/*
 * The controller's microphone: what it sends as media, decoded for the USB microphone.
 *
 * The controller sends it once asked with DisplayToController #5 (true) (rtc.c), as SRTP on the
 * display link's selected pair (rtc_link.c decrypts it with the controller's DTLS-SRTP keys),
 * under the display offer's audio_ssrc: Opus PT 111, one 20 ms frame per packet, with DTX
 * (3-byte frames, about 7 a second, while nobody talks). Seen on firmware 316382. The game
 * session is listened to as well, in case another firmware sends it there.
 *
 * The rtc task hands each packet to mic_on_rtp(), which only copies it into a short queue; a
 * task of its own decodes (Opus wants more stack than the rtc task has) into mono 48 kHz PCM
 * for the callback, concealing up to MAX_PLC lost frames on a gap in the sequence numbers.
 * Every STATS_MS it logs what came in, and the loudest sample, so silence is told apart from
 * nothing.
 *
 * Push to talk (stadia_config_t.mic_push_to_talk): game.c reports the Assistant button through
 * mic_set_talk(), and packets are only queued while it is held, plus TALK_TAIL_MS after it is
 * let go so the last word is not cut off. rtc.c asks for the stream on the press.
 */
#include "stadia_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mic.h"
#include "opus.h"
#include "stadia_port.h"

static const char *TAG = "mic";

#define RATE 48000
#define MAX_FRAME 5760 /* 120 ms, the longest Opus frame */
#define MAX_PKT 1500
#define QUEUE_LEN 8
#define MAX_PLC 3 /* lost frames concealed on a gap; a longer one is just skipped */
#define STATS_MS 10000
#define TALK_TAIL_MS 300

typedef struct {
    uint16_t len;
    uint16_t seq;
    uint8_t pt;
    uint8_t link; /* 0 display, 1 game */
    uint8_t data[MAX_PKT];
} pkt_t;

static struct {
    /* the queue, under port_lock */
    pkt_t *q; /* QUEUE_LEN, on the heap */
    int head, count;
    uint32_t queued[2], overflow;
    int64_t last_rx_ms; /* the last packet received, queued or not */
    uint32_t muted;     /* packets dropped while not talking */
    uint32_t t_packets, t_muted, t_overflow; /* from power-up, for the console (mic_get_stats) */
    bool talk_held;
    int64_t talk_released_ms;
    port_event_t *ev;
    /* the callback */
    stadia_mic_cb_t cb;
    void *arg;
    /* the mic task */
    OpusDecoder *dec;
    bool dec_failed;
    int32_t last_seq; /* -1: the next packet starts a stream */
    uint8_t stream_link;
    int64_t last_pkt_ms;
    uint32_t decoded, errors, lost, samples;
    int peak;
    int64_t stats_at;
    int logged;
    volatile uint32_t t_decoded, t_lost, t_errors;
    volatile int t_peak;
} M;

void stadia_set_mic_callback(stadia_mic_cb_t cb, void *arg)
{
    port_lock();
    M.cb = cb;
    M.arg = arg;
    port_unlock();
}

void mic_on_rtp(rtc_link_t *L, int link, const uint8_t *rtp, size_t n)
{
    uint8_t pt;
    const uint8_t *payload;
    size_t plen;
    if (!M.q || !rtp_payload(rtp, n, &pt, &payload, &plen) || !plen || plen > MAX_PKT) return;
    int64_t now = port_now_ms();
    port_lock();
    M.last_rx_ms = now;
    if (g_cfg.mic_push_to_talk && !M.talk_held && now - M.talk_released_ms >= TALK_TAIL_MS) {
        M.muted++;
        M.t_muted++;
        port_unlock();
        return;
    }
    if (M.count == QUEUE_LEN) { /* the decoder fell behind: the oldest goes */
        M.head = (M.head + 1) % QUEUE_LEN;
        M.count--;
        M.overflow++;
        M.t_overflow++;
    }
    pkt_t *p = &M.q[(M.head + M.count) % QUEUE_LEN];
    p->len = (uint16_t)plen;
    p->seq = (uint16_t)(rtp[2] << 8 | rtp[3]);
    p->pt = pt;
    p->link = (uint8_t)link;
    memcpy(p->data, payload, plen);
    M.count++;
    M.queued[link ? 1 : 0]++;
    M.t_packets++;
    port_unlock();
    port_event_set(M.ev);
}

static bool decoder_ready(void)
{
    if (M.dec) return true;
    if (M.dec_failed) return false;
    int size = opus_decoder_get_size(1);
    OpusDecoder *dec = port_alloc_fast((size_t)size);
    if (!dec) dec = malloc((size_t)size);
    int err = dec ? opus_decoder_init(dec, RATE, 1) : OPUS_ALLOC_FAIL;
    if (err != OPUS_OK) {
        SLOGE(TAG, "Opus decoder: %s", opus_strerror(err));
        free(dec);
        M.dec_failed = true;
        return false;
    }
    M.dec = dec;
    return true;
}

static void deliver(const int16_t *pcm, int frames)
{
    for (int i = 0; i < frames; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > M.peak) M.peak = v;
        if (v > M.t_peak) M.t_peak = v;
    }
    M.samples += (uint32_t)frames;
    port_lock();
    stadia_mic_cb_t cb = M.cb;
    void *arg = M.arg;
    port_unlock();
    if (cb) cb(pcm, (size_t)frames, arg);
}

static void decode(const pkt_t *p, int16_t *pcm)
{
    int64_t now = port_now_ms();
    /* one stream at a time: the first link it came on, until that one goes quiet for a second */
    if (M.last_seq >= 0 && p->link != M.stream_link && now - M.last_pkt_ms < 1000) return;
    if (M.last_seq < 0 || p->link != M.stream_link || now - M.last_pkt_ms > 1000) {
        SLOGI(TAG, "microphone stream on the %s link: PT %u, %u-byte packets, %d samples each",
              p->link ? "game" : "display", p->pt, p->len, opus_packet_get_nb_samples(p->data, p->len, RATE));
        M.stream_link = p->link;
        M.last_seq = -1;
    }
    M.last_pkt_ms = now;
    if (!decoder_ready()) return;
    if (M.last_seq >= 0) {
        int gap = (uint16_t)(p->seq - M.last_seq) - 1;
        if (gap < 0 || gap > 1000) return; /* late or repeated */
        M.lost += (uint32_t)gap;
        M.t_lost += (uint32_t)gap;
        int frame = opus_packet_get_nb_samples(p->data, p->len, RATE);
        for (int i = 0; i < gap && i < MAX_PLC && frame > 0; i++) {
            int n = opus_decode(M.dec, NULL, 0, pcm, frame, 0);
            if (n > 0) deliver(pcm, n);
        }
    }
    M.last_seq = p->seq;
    int n = opus_decode(M.dec, p->data, p->len, pcm, MAX_FRAME, 0);
    if (n <= 0) {
        M.t_errors++;
        if (M.errors++ < 5) {
            SLOGW(TAG, "not Opus? %s (PT %u, %u bytes, first %02x %02x %02x %02x)", opus_strerror(n), p->pt, p->len,
                  p->data[0], p->len > 1 ? p->data[1] : 0, p->len > 2 ? p->data[2] : 0, p->len > 3 ? p->data[3] : 0);
        }
        return;
    }
    M.decoded++;
    M.t_decoded++;
    deliver(pcm, n);
}

static void stats(void)
{
    int64_t now = port_now_ms();
    if (!M.stats_at) M.stats_at = now;
    if (now - M.stats_at < STATS_MS) return;
    port_lock();
    uint32_t d = M.queued[0], g = M.queued[1], over = M.overflow, muted = M.muted;
    M.queued[0] = M.queued[1] = M.overflow = M.muted = 0;
    port_unlock();
    if (muted) SLOGI(TAG, "%u packets muted in %d s (push to talk: Assistant not held)", (unsigned)muted,
                     (int)((now - M.stats_at) / 1000));
    if (d || g || M.errors) {
        int db = M.peak ? (int)lroundf(20.0f * log10f(M.peak / 32768.0f)) : -96;
        SLOGI(TAG, "%u packets in %d s (display %u, game %u), %u decoded, %u lost, %u errors, %u dropped; %u ms of "
                   "sound, peak %d dBFS",
              (unsigned)(d + g), (int)((now - M.stats_at) / 1000), (unsigned)d, (unsigned)g, (unsigned)M.decoded,
              (unsigned)M.lost, (unsigned)M.errors, (unsigned)over, (unsigned)(M.samples / 48), db);
    }
    M.decoded = M.lost = M.errors = M.samples = 0;
    M.peak = 0;
    M.stats_at = now;
}

static void mic_task(void *arg)
{
    /* On the heap, not in .bss: on v1.x P4 chips .bss comes out of the small RAM pool the system
     * starts from, and these 13 KB there stopped the board booting (see c6_update.c). PSRAM:
     * the decoder's state stays in internal RAM, these only carry a packet and a frame. */
    pkt_t *pp = port_alloc_bulk(sizeof(pkt_t));
    int16_t *pcm = port_alloc_bulk(MAX_FRAME * sizeof(int16_t));
    if (!pp || !pcm) {
        SLOGE(TAG, "no memory for the microphone decoder");
        free(pp);
        free(pcm);
        return;
    }
    pkt_t *p = pp;
    for (;;) {
        port_event_wait(M.ev, 1000);
        for (;;) {
            port_lock();
            bool have = M.count > 0;
            if (have) {
                *p = M.q[M.head];
                M.head = (M.head + 1) % QUEUE_LEN;
                M.count--;
            }
            port_unlock();
            if (!have) break;
            decode(p, pcm);
        }
        stats();
    }
}

int64_t mic_last_packet_ms(void)
{
    port_lock();
    int64_t t = M.last_rx_ms;
    port_unlock();
    return t;
}

void mic_set_talk(bool held)
{
    port_lock();
    bool was = M.talk_held;
    M.talk_held = held;
    if (was && !held) M.talk_released_ms = port_now_ms();
    port_unlock();
    if (g_cfg.mic_push_to_talk && was != held) SLOGI(TAG, "push to talk: %s", held ? "talking" : "released");
}

void stadia_mic_set_push_to_talk(bool on)
{
    port_lock();
    bool was = g_cfg.mic_push_to_talk;
    g_cfg.mic_push_to_talk = on;
    port_unlock();
    if (was != on) SLOGI(TAG, "push to talk %s", on ? "on" : "off");
}

bool stadia_mic_push_to_talk(void)
{
    port_lock();
    bool on = g_cfg.mic_push_to_talk;
    port_unlock();
    return on;
}

bool mic_talking(void)
{
    port_lock();
    bool on = M.talk_held || (M.talk_released_ms && port_now_ms() - M.talk_released_ms < TALK_TAIL_MS);
    port_unlock();
    return on;
}

bool mic_init(void)
{
    M.last_seq = -1;
    M.q = port_alloc_bulk(QUEUE_LEN * sizeof(pkt_t)); /* 12 KB of packets: PSRAM */
    M.ev = port_event_new();
    if (!M.q || !M.ev) {
        SLOGE(TAG, "no memory for the microphone queue");
        free(M.q);
        M.q = NULL;
        return false;
    }
    /* below the rtc task and the headphones' encoder: the microphone can wait a few ms */
    return port_spawn_low(mic_task, NULL, "mic", STADIA_AUDIO_STACK);
}

void mic_get_stats(stadia_stats_t *st)
{
    port_lock();
    st->mic_packets = M.t_packets;
    st->mic_muted = M.t_muted;
    st->mic_dropped = M.t_overflow;
    int64_t last = M.last_rx_ms;
    port_unlock();
    st->mic_decoded = M.t_decoded;
    st->mic_lost = M.t_lost;
    st->mic_errors = M.t_errors;
    st->mic_peak = M.t_peak;
    M.t_peak = 0;
    st->mic_talking = mic_talking();
    int64_t age = port_now_ms() - last;
    st->mic_age_ms = !last ? -1 : age > INT32_MAX ? INT32_MAX : (int32_t)age;
}
