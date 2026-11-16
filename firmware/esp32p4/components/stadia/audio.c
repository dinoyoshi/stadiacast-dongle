/*
 * Headset audio (port of audio.py and docs/protocol.md "Audio to the headset jack"):
 *
 *   48 kHz stereo PCM -> Opus (20 ms, 64 kbps, restricted low delay) -> RTP (PT 111, own SSRC
 *   and sequence, timestamp on the 20 ms grid) -> SRTP -> UDP on the display link's selected pair
 *
 * The producer (the USB audio task on the board, a tone thread in the host test) calls
 * stadia_audio_pcm(); every 960 frames it encodes one Opus frame there and queues it. The rtc
 * task takes the queue in audio_tick() and sends, but only once the controller's audio route is
 * open: SetAudioModeRequest(mode 2) went out at READY and was answered (rtc.c).
 *
 * The frames follow the producer's clock, which on the board is the USB host's: one frame per
 * 20 ms of audio it played. A pause (the host stopped streaming) leaves a hole in the timestamps
 * of the length it lasted, so the controller's jitter buffer stays on the same grid.
 *
 * The rtc task sends them on a 20 ms grid of its own (audio.py's pacer), PACE_HEADROOM_US behind
 * the first frame, so the encoder's 5-11 ms of jitter does not reach the air. A slot with no
 * frame is skipped (an underrun); more than 10 slots behind restarts the grid.
 */
#include "stadia_internal.h"

#include <stdlib.h>
#include <string.h>

#include "opus.h"
#include "audio.h"
#include "stadia_port.h"

static const char *TAG = "audio";

#define FRAME_SAMPLES 960 /* 20 ms at 48 kHz */
#define CHANNELS 2
#define BITRATE_KBPS 96 /* when the config gives none */
#define PAYLOAD_TYPE 111
#define MAX_OPUS 600
#define QUEUE_LEN 6    /* 120 ms: past it the oldest frame goes, as in audio.py */
#define GAP_US 100000  /* a pause longer than this resyncs the timestamps to the wall clock */
#define STATS_MS 10000
#define SLOT_US 20000
#define PACE_HEADROOM_US 15000

typedef struct {
    uint16_t len;
    bool marker;
    uint32_t ts;
    uint8_t data[MAX_OPUS];
} frame_t;

static struct {
    /* producer side */
    OpusEncoder *enc;
    bool enc_failed;
    int complexity;
    int kbps;
    int16_t acc[FRAME_SAMPLES * CHANNELS];
    size_t nacc;
    uint32_t ts;
    int64_t last_frame_us;
    /* shared, under port_lock */
    frame_t *q; /* QUEUE_LEN frames, in PSRAM: out of .bss (see mic.c) and internal RAM */
    int head, count;
    uint32_t encoded, trimmed, enc_us_sum, enc_us_max, bytes;
    int64_t last_queued_ms;
    /* rtc task side */
    volatile bool route; /* frames are wanted (the controller's audio route is open) */
    uint16_t seq;
    uint32_t ssrc;
    uint32_t sent, dropped, underruns, resyncs;
    int64_t stats_at;
    int64_t next_slot_us; /* the pacer's next send time, 0 = waiting for a first frame */
    /* from power-up, for the console (audio_get_stats) */
    volatile int want_complexity; /* 0: no change asked for */
    volatile int want_kbps;       /* 0: no change asked for */
    uint32_t t_encoded, t_trimmed, t_enc_us_max; /* under port_lock */
    uint64_t t_bytes, t_enc_us;
    volatile uint32_t t_sent, t_dropped, t_underruns, t_resyncs; /* the rtc task's */
} A;

void audio_init(const stadia_config_t *cfg)
{
    A.complexity = cfg->audio_complexity ? cfg->audio_complexity : 8;
    A.kbps = cfg->audio_kbps ? cfg->audio_kbps : BITRATE_KBPS;
    if (!A.q) A.q = port_alloc_bulk(QUEUE_LEN * sizeof(frame_t));
    if (!A.q) {
        SLOGE(TAG, "no memory for the audio queue");
        A.enc_failed = true; /* no frames to queue: encoder_ready() says no */
    }
    uint8_t r[4];
    port_random(r, sizeof(r));
    A.ssrc = 0x51A00000u | (uint32_t)(r[0] << 8 | r[1]); /* its own SSRC, not the input ACKs' */
    A.seq = (uint16_t)(r[2] << 8 | r[3]);
}

static bool encoder_ready(void)
{
    if (A.enc) return true;
    if (A.enc_failed) return false;
    /* the encoder's state in internal RAM: from PSRAM, encoding takes noticeably longer */
    int size = opus_encoder_get_size(CHANNELS);
    OpusEncoder *enc = port_alloc_fast((size_t)size);
    if (!enc) enc = malloc((size_t)size);
    int err = enc ? opus_encoder_init(enc, 48000, CHANNELS, OPUS_APPLICATION_RESTRICTED_LOWDELAY) : OPUS_ALLOC_FAIL;
    if (err != OPUS_OK) {
        SLOGE(TAG, "Opus encoder: %s", opus_strerror(err));
        free(enc);
        A.enc_failed = true;
        return false;
    }
    A.enc = enc;
    opus_encoder_ctl(A.enc, OPUS_SET_BITRATE(A.kbps * 1000));
    opus_encoder_ctl(A.enc, OPUS_SET_COMPLEXITY(A.complexity));
    SLOGI(TAG, "Opus %s: 48 kHz stereo, 20 ms, %d kbps, complexity %d, %d bytes of state", opus_get_version_string(),
          A.kbps, A.complexity, opus_encoder_get_size(CHANNELS));
    return true;
}

static void encode_frame(void)
{
    int64_t now = port_now_us();
    bool marker = false;
    if (!A.last_frame_us) {
        marker = true;
    } else if (now - A.last_frame_us > GAP_US) {
        /* a pause: skip the slots it took, so the next frame lands where the clock says */
        A.ts += (uint32_t)((now - A.last_frame_us + 10000) / 20000) * FRAME_SAMPLES;
        marker = true;
    } else {
        A.ts += FRAME_SAMPLES;
    }
    A.last_frame_us = now;

    int want = A.want_complexity;
    if (want && want != A.complexity) {
        A.complexity = want;
        opus_encoder_ctl(A.enc, OPUS_SET_COMPLEXITY(want));
        SLOGI(TAG, "Opus complexity %d", want);
    }
    int kbps = A.want_kbps;
    if (kbps && kbps != A.kbps) {
        A.kbps = kbps;
        opus_encoder_ctl(A.enc, OPUS_SET_BITRATE(kbps * 1000));
        SLOGI(TAG, "Opus bitrate %d kbps", kbps);
    }
    uint8_t out[MAX_OPUS];
    int n = opus_encode(A.enc, A.acc, FRAME_SAMPLES, out, sizeof(out));
    uint32_t took = (uint32_t)(port_now_us() - now);
    if (n <= 0) {
        SLOGW(TAG, "opus_encode: %s", opus_strerror(n));
        return;
    }
    port_lock();
    if (A.count == QUEUE_LEN) { /* the rtc task fell behind: drop the oldest */
        A.head = (A.head + 1) % QUEUE_LEN;
        A.count--;
        A.trimmed++;
        A.t_trimmed++;
    }
    frame_t *f = &A.q[(A.head + A.count) % QUEUE_LEN];
    f->len = (uint16_t)n;
    f->marker = marker;
    f->ts = A.ts;
    memcpy(f->data, out, (size_t)n);
    A.count++;
    A.encoded++;
    A.bytes += (uint32_t)n;
    A.enc_us_sum += took;
    A.t_encoded++;
    A.t_bytes += (uint32_t)n;
    A.t_enc_us += took;
    if (took > A.t_enc_us_max) A.t_enc_us_max = took;
    if (took > A.enc_us_max) A.enc_us_max = took;
    A.last_queued_ms = port_now_ms();
    port_unlock();
}

void stadia_audio_pcm(const int16_t *pcm, size_t frames)
{
    if (!A.route) { /* nobody to play it to: skip the encoder's work, start the next frame fresh */
        A.nacc = 0;
        return;
    }
    if (!encoder_ready()) return;
    while (frames) {
        size_t take = FRAME_SAMPLES - A.nacc;
        if (take > frames) take = frames;
        memcpy(A.acc + A.nacc * CHANNELS, pcm, take * CHANNELS * sizeof(int16_t));
        A.nacc += take;
        pcm += take * CHANNELS;
        frames -= take;
        if (A.nacc == FRAME_SAMPLES) {
            encode_frame();
            A.nacc = 0;
        }
    }
}

void audio_set_route(bool open)
{
    if (A.route != open) SLOGI(TAG, "headset audio route %s", open ? "open: sending" : "closed");
    A.route = open;
    if (!open) {
        port_lock();
        A.count = 0;
        port_unlock();
        A.next_slot_us = 0;
    }
}

int audio_poll_ms(void)
{
    return A.route && port_now_ms() - A.last_queued_ms < 1000 ? 2 : 20;
}

/* Whether a frame is due now: the 20 ms grid, started PACE_HEADROOM_US after the first frame. */
static bool slot_due(void)
{
    int64_t now = port_now_us();
    port_lock();
    int count = A.count;
    port_unlock();
    if (!A.next_slot_us) {
        if (!count) return false;
        A.next_slot_us = now + PACE_HEADROOM_US;
        return false;
    }
    if (now < A.next_slot_us) return false;
    if (now - A.next_slot_us > 10 * SLOT_US) { /* a stall: start a new grid rather than burst */
        A.resyncs++;
        A.t_resyncs++;
        A.next_slot_us = count ? now + PACE_HEADROOM_US : 0;
        return false;
    }
    A.next_slot_us += SLOT_US;
    if (!count) {
        A.underruns++;
        A.t_underruns++;
        if (port_now_ms() - A.last_queued_ms > 200) A.next_slot_us = 0; /* the stream stopped */
        return false;
    }
    return true;
}

void audio_tick(rtc_link_t *L)
{
    while (slot_due()) {
        frame_t f;
        port_lock();
        bool have = A.count > 0;
        if (have) {
            f.len = A.q[A.head].len;
            f.marker = A.q[A.head].marker;
            f.ts = A.q[A.head].ts;
            memcpy(f.data, A.q[A.head].data, f.len);
            A.head = (A.head + 1) % QUEUE_LEN;
            A.count--;
        }
        port_unlock();
        if (!have) break;

        uint8_t pkt[12 + MAX_OPUS + SRTP_TAG_LEN];
        pkt[0] = 0x80;
        pkt[1] = (uint8_t)((f.marker ? 0x80 : 0) | PAYLOAD_TYPE);
        pkt[2] = A.seq >> 8, pkt[3] = A.seq & 0xFF;
        pkt[4] = f.ts >> 24, pkt[5] = (f.ts >> 16) & 0xFF, pkt[6] = (f.ts >> 8) & 0xFF, pkt[7] = f.ts & 0xFF;
        pkt[8] = A.ssrc >> 24, pkt[9] = (A.ssrc >> 16) & 0xFF, pkt[10] = (A.ssrc >> 8) & 0xFF, pkt[11] = A.ssrc & 0xFF;
        memcpy(pkt + 12, f.data, f.len);
        if (rtc_link_send_srtp(L, pkt, 12 + f.len, sizeof(pkt))) {
            A.seq++;
            A.sent++;
            A.t_sent++;
        } else {
            A.dropped++;
            A.t_dropped++;
        }
    }

    int64_t now = port_now_ms();
    if (!A.stats_at) A.stats_at = now;
    if (now - A.stats_at >= STATS_MS) {
        port_lock();
        uint32_t enc = A.encoded, trimmed = A.trimmed, sum = A.enc_us_sum, max = A.enc_us_max, bytes = A.bytes;
        A.encoded = A.trimmed = A.enc_us_sum = A.enc_us_max = A.bytes = 0;
        port_unlock();
        if (enc || A.sent || A.dropped) {
            SLOGI(TAG,
                  "%u frames sent in %d s (%u B avg), %u not sent, %u trimmed, %u underruns, %u resyncs; "
                  "encode avg %u us, max %u us",
                  (unsigned)A.sent, (int)((now - A.stats_at) / 1000), (unsigned)(enc ? bytes / enc : 0),
                  (unsigned)A.dropped, (unsigned)trimmed, (unsigned)A.underruns, (unsigned)A.resyncs,
                  (unsigned)(enc ? sum / enc : 0), (unsigned)max);
        }
        A.sent = A.dropped = A.underruns = A.resyncs = 0;
        A.stats_at = now;
    }
}

void stadia_audio_set_complexity(int complexity)
{
    if (complexity < 1) complexity = 1;
    if (complexity > 10) complexity = 10;
    A.want_complexity = complexity;
    if (!A.enc) A.complexity = complexity; /* before the first frame: the encoder starts with it */
}

void stadia_audio_set_bitrate(int kbps)
{
    if (kbps < STADIA_AUDIO_KBPS_MIN) kbps = STADIA_AUDIO_KBPS_MIN;
    if (kbps > STADIA_AUDIO_KBPS_MAX) kbps = STADIA_AUDIO_KBPS_MAX;
    A.want_kbps = kbps;
    if (!A.enc) A.kbps = kbps; /* before the first frame: the encoder starts with it */
}

void audio_get_stats(stadia_stats_t *st)
{
    st->audio_route = A.route;
    st->audio_complexity = (uint8_t)(A.want_complexity ? A.want_complexity : A.complexity);
    st->audio_kbps = (uint8_t)(A.want_kbps ? A.want_kbps : A.kbps);
    port_lock();
    st->audio_encoded = A.t_encoded;
    st->audio_trimmed = A.t_trimmed;
    st->audio_bytes = A.t_bytes;
    st->audio_enc_us = A.t_enc_us;
    st->audio_enc_us_max = A.t_enc_us_max;
    A.t_enc_us_max = 0;
    port_unlock();
    st->audio_sent = A.t_sent;
    st->audio_not_sent = A.t_dropped;
    st->audio_underruns = A.t_underruns;
    st->audio_resyncs = A.t_resyncs;
}
