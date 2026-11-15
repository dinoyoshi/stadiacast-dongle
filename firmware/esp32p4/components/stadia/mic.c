/*
 * The controller's microphone: what it sends as media, decoded for the USB microphone.
 *
 * Nothing about this stream is documented (docs/protocol.md only names the display as "the RTP
 * receiver of the controller's mic and input streams"). What the code assumes, and logs so a
 * board run can tell:
 *
 *   - it arrives as SRTP on a link's selected pair (rtc_link.c decrypts it with the
 *     controller's DTLS-SRTP keys), on the display link, the game session or both
 *   - its payload is Opus at 48 kHz (the codec the offers list as the controller's audio, with
 *     its own audio_ssrc), one frame per packet
 *
 * The rtc task hands each packet to mic_on_rtp(), which only copies it into a short queue; a
 * task of its own decodes (Opus wants more stack than the rtc task has) into mono 48 kHz PCM
 * for the callback, concealing up to MAX_PLC lost frames on a gap in the sequence numbers.
 * Every STATS_MS it logs what came in, and the loudest sample, so silence is told apart from
 * nothing.
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
    port_lock();
    if (M.count == QUEUE_LEN) { /* the decoder fell behind: the oldest goes */
        M.head = (M.head + 1) % QUEUE_LEN;
        M.count--;
        M.overflow++;
    }
    pkt_t *p = &M.q[(M.head + M.count) % QUEUE_LEN];
    p->len = (uint16_t)plen;
    p->seq = (uint16_t)(rtp[2] << 8 | rtp[3]);
    p->pt = pt;
    p->link = (uint8_t)link;
    memcpy(p->data, payload, plen);
    M.count++;
    M.queued[link ? 1 : 0]++;
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
        int frame = opus_packet_get_nb_samples(p->data, p->len, RATE);
        for (int i = 0; i < gap && i < MAX_PLC && frame > 0; i++) {
            int n = opus_decode(M.dec, NULL, 0, pcm, frame, 0);
            if (n > 0) deliver(pcm, n);
        }
    }
    M.last_seq = p->seq;
    int n = opus_decode(M.dec, p->data, p->len, pcm, MAX_FRAME, 0);
    if (n <= 0) {
        if (M.errors++ < 5) {
            SLOGW(TAG, "not Opus? %s (PT %u, %u bytes, first %02x %02x %02x %02x)", opus_strerror(n), p->pt, p->len,
                  p->data[0], p->len > 1 ? p->data[1] : 0, p->len > 2 ? p->data[2] : 0, p->len > 3 ? p->data[3] : 0);
        }
        return;
    }
    M.decoded++;
    deliver(pcm, n);
}

static void stats(void)
{
    int64_t now = port_now_ms();
    if (!M.stats_at) M.stats_at = now;
    if (now - M.stats_at < STATS_MS) return;
    port_lock();
    uint32_t d = M.queued[0], g = M.queued[1], over = M.overflow;
    M.queued[0] = M.queued[1] = M.overflow = 0;
    port_unlock();
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
     * starts from, and these 13 KB there stopped the board booting (see c6_update.c). */
    pkt_t *pp = malloc(sizeof(pkt_t));
    int16_t *pcm = malloc(MAX_FRAME * sizeof(int16_t));
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

bool mic_init(void)
{
    M.last_seq = -1;
    M.q = calloc(QUEUE_LEN, sizeof(pkt_t));
    M.ev = port_event_new();
    if (!M.q || !M.ev) {
        SLOGE(TAG, "no memory for the microphone queue");
        free(M.q);
        M.q = NULL;
        return false;
    }
    return port_spawn(mic_task, NULL, "mic", STADIA_AUDIO_STACK);
}
