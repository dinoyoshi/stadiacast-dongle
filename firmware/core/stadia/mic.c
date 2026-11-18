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
 *
 * Gain: the same request gives the headset's mic when one is in the controller's jack and the
 * controller's built-in mic (the Assistant's) when none is; the stream does not say which. The
 * built-in one is much quieter, so each has its own gain (stadia_mic_set_gain), picked by what
 * rtc.c last heard about the jack (mic_set_headset). Above 0 dB a peak limiter keeps loud speech
 * under -1 dBFS instead of clipping it.
 *
 * Several players: each controller's stream has its own decoder, push to talk (its own Assistant
 * button), jack and gain, and goes into a FIFO of its own; the mixer adds them up into the one
 * stream the callback gets. It sends a 20 ms chunk once every player who is talking has one, or
 * once someone is JITTER_MS ahead (a late player's missing part is silence then). With one
 * player talking that is as soon as its frame is decoded: no delay added.
 */
#include "stadia_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mic.h"
#include "opus.h"
#include "stadia_port.h"

static const char *TAG = "mic";

#define RATE 48000
#define MAX_FRAME 5760 /* 120 ms, the longest Opus frame */
#define MAX_PKT 1500
#define QUEUE_LEN 12
#define MAX_PLC 3 /* lost frames concealed on a gap; a longer one is just skipped */
#define STATS_MS 10000
#define TALK_TAIL_MS 300
#define DTX_MAX 3 /* bytes: a DTX frame while nobody talks, cheap to decode */
#define BUSY_MS 300
#define FIFO_LEN 7680 /* 160 ms per player, waiting for the mixer */
#define CHUNK 960     /* the mixer's piece: 20 ms */
#define JITTER (2 * CHUNK) /* how far one player may run ahead of another who is talking */
#define WAIT_MS 40    /* a player decoded this recently counts as talking (the mixer waits for them) */
#define LIMIT 29205.0f      /* -1 dBFS: the limiter's ceiling */
#define LIMIT_REL 0.99979f  /* its release, per sample: about 100 ms at 48 kHz */

typedef struct {
    uint16_t len;
    uint16_t seq;
    uint8_t pt;
    uint8_t link; /* 0 display, 1 game */
    uint8_t slot; /* the player */
    uint8_t data[MAX_PKT];
} pkt_t;

/* One player's microphone. */
typedef struct {
    /* under port_lock */
    bool talk_held;
    int64_t talk_released_ms;
    int64_t last_rx_ms;     /* the last packet received, queued or not */
    int64_t last_speech_ms; /* the last queued packet bigger than a DTX one */
    volatile int8_t headset; /* 1 in, 0 out, -1 not heard yet (the headset's gain then) */
    /* the mic task's */
    OpusDecoder *dec;
    bool dec_failed;
    int32_t last_seq; /* -1: the next packet starts a stream */
    uint8_t stream_link;
    int64_t last_pkt_ms;
    int64_t last_dec_ms;
    int applied_db; /* the gain in use, and as a factor */
    float gain;
    float env;      /* the limiter's envelope */
    int16_t *fifo;  /* FIFO_LEN, decoded and gained, waiting for the mixer */
    int rd, fill;
} src_t;

static struct {
    /* the queue, under port_lock */
    pkt_t *q; /* QUEUE_LEN, on the heap */
    int head, count;
    uint32_t queued[2], overflow;
    uint32_t muted;     /* packets dropped while not talking */
    uint32_t t_packets, t_muted, t_overflow; /* from power-up, for the console (mic_get_stats) */
    port_event_t *ev;
    /* the callback */
    stadia_mic_cb_t cb;
    void *arg;
    /* the mic task */
    uint32_t decoded, errors, lost, samples;
    int peak;
    int64_t stats_at;
    volatile uint32_t t_decoded, t_lost, t_errors;
    volatile int t_peak;
    /* gain: dB for a headset and for the built-in mic, set by the console */
    volatile int8_t gain_db[2]; /* [0] headset, [1] built-in */
    src_t S[STADIA_MAX_CONTROLLERS];
} M;

void stadia_set_mic_callback(stadia_mic_cb_t cb, void *arg)
{
    port_lock();
    M.cb = cb;
    M.arg = arg;
    port_unlock();
}

static src_t *src(int slot)
{
    return slot >= 0 && slot < STADIA_MAX_CONTROLLERS ? &M.S[slot] : NULL;
}

void mic_on_rtp(rtc_link_t *L, int link, const uint8_t *rtp, size_t n)
{
    uint8_t pt;
    const uint8_t *payload;
    size_t plen;
    src_t *S = src(L->slot);
    if (!M.q || !S || !rtp_payload(rtp, n, &pt, &payload, &plen) || !plen || plen > MAX_PKT) return;
    int64_t now = port_now_ms();
    port_lock();
    S->last_rx_ms = now;
    if (g_cfg.mic_push_to_talk && !S->talk_held && now - S->talk_released_ms >= TALK_TAIL_MS) {
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
    p->slot = (uint8_t)L->slot;
    memcpy(p->data, payload, plen);
    M.count++;
    if (plen > DTX_MAX) S->last_speech_ms = now;
    M.queued[link ? 1 : 0]++;
    M.t_packets++;
    port_unlock();
    port_event_set(M.ev);
}

static bool decoder_ready(src_t *S)
{
    if (S->dec) return true;
    if (S->dec_failed) return false;
    int size = opus_decoder_get_size(1);
    OpusDecoder *dec = port_alloc_fast((size_t)size);
    if (!dec) dec = malloc((size_t)size);
    int err = dec ? opus_decoder_init(dec, RATE, 1) : OPUS_ALLOC_FAIL;
    if (!S->fifo) S->fifo = port_alloc_bulk(FIFO_LEN * sizeof(int16_t)); /* only copied through: PSRAM */
    if (err == OPUS_OK && !S->fifo) err = OPUS_ALLOC_FAIL;
    if (err != OPUS_OK) {
        SLOGE(TAG, "player %d's Opus decoder: %s", (int)(S - M.S) + 1, opus_strerror(err));
        free(dec);
        S->dec_failed = true;
        return false;
    }
    S->dec = dec;
    return true;
}

/* The gain for the mic this player's controller is using, with the limiter when it boosts. */
static void apply_gain(src_t *S, int16_t *pcm, int frames)
{
    int db = M.gain_db[S->headset == 0];
    if (db != S->applied_db) {
        char who[16] = "";
        if (S != M.S) snprintf(who, sizeof(who), "player %d: ", (int)(S - M.S) + 1);
        SLOGI(TAG, "%sgain %+d dB (%s)", who, db, S->headset == 0 ? "built-in microphone, no headset" : "headset microphone");
        S->applied_db = db;
        S->gain = powf(10.0f, db / 20.0f);
        S->env = 0;
    }
    if (!db) return;
    float g = S->gain, env = S->env;
    for (int i = 0; i < frames; i++) {
        float y = pcm[i] * g;
        float a = fabsf(y);
        env *= LIMIT_REL;
        if (a > env) env = a; /* instant attack: env >= a, so the output never passes LIMIT */
        if (env > LIMIT) y *= LIMIT / env;
        pcm[i] = (int16_t)lrintf(y > 32767.0f ? 32767.0f : y < -32768.0f ? -32768.0f : y);
    }
    S->env = env;
}

/* Decoded samples into this player's FIFO for the mixer (the oldest go if it is full). */
static void push(src_t *S, int16_t *pcm, int n)
{
    apply_gain(S, pcm, n);
    S->last_dec_ms = port_now_ms();
    M.samples += (uint32_t)n;
    if (n > FIFO_LEN) {
        pcm += n - FIFO_LEN;
        n = FIFO_LEN;
    }
    int over = S->fill + n - FIFO_LEN;
    if (over > 0) {
        S->rd = (S->rd + over) % FIFO_LEN;
        S->fill -= over;
    }
    int wr = (S->rd + S->fill) % FIFO_LEN;
    for (int i = 0; i < n; i++) {
        S->fifo[wr] = pcm[i];
        if (++wr == FIFO_LEN) wr = 0;
    }
    S->fill += n;
}

static void deliver(const int16_t *pcm, int frames)
{
    for (int i = 0; i < frames; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > M.peak) M.peak = v;
        if (v > M.t_peak) M.t_peak = v;
    }
    port_lock();
    stadia_mic_cb_t cb = M.cb;
    void *arg = M.arg;
    port_unlock();
    if (cb) cb(pcm, (size_t)frames, arg);
}

/* Mix what the players' FIFOs hold into CHUNK-sample pieces for the callback, while one is due
 * (see the top). True if something is left over for later. */
static bool mix(int16_t *out)
{
    for (;;) {
        int64_t now = port_now_ms();
        int active = 0, ready = 0, most = 0;
        int64_t newest = 0;
        for (int s = 0; s < g_nctl; s++) {
            src_t *S = &M.S[s];
            if (!S->fifo) continue;
            if (S->last_dec_ms > newest) newest = S->last_dec_ms;
            if (!S->fill && !(S->last_dec_ms && now - S->last_dec_ms < WAIT_MS)) continue;
            active++;
            if (S->fill >= CHUNK) ready++;
            if (S->fill > most) most = S->fill;
        }
        if (!most) return false;
        bool stale = now - newest >= WAIT_MS; /* everyone stopped: let the last bit out */
        if (ready < active && most < CHUNK + JITTER && !stale) return true;
        int n = most < CHUNK ? most : CHUNK;
        int32_t acc[CHUNK];
        memset(acc, 0, sizeof(int32_t) * (size_t)n);
        for (int s = 0; s < g_nctl; s++) {
            src_t *S = &M.S[s];
            if (!S->fifo || !S->fill) continue;
            int take = S->fill < n ? S->fill : n;
            for (int i = 0; i < take; i++) {
                acc[i] += S->fifo[S->rd];
                if (++S->rd == FIFO_LEN) S->rd = 0;
            }
            S->fill -= take;
        }
        for (int i = 0; i < n; i++) out[i] = acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : (int16_t)acc[i];
        deliver(out, n);
    }
}

static void decode(const pkt_t *p, int16_t *pcm)
{
    src_t *S = src(p->slot);
    if (!S) return;
    int64_t now = port_now_ms();
    /* one stream per player: the first link it came on, until that one goes quiet for a second */
    if (S->last_seq >= 0 && p->link != S->stream_link && now - S->last_pkt_ms < 1000) return;
    if (S->last_seq < 0 || p->link != S->stream_link || now - S->last_pkt_ms > 1000) {
        char who[20] = "";
        if (p->slot) snprintf(who, sizeof(who), "player %d's ", p->slot + 1);
        SLOGI(TAG, "%smicrophone stream on the %s link: PT %u, %u-byte packets, %d samples each", who,
              p->link ? "game" : "display", p->pt, p->len, opus_packet_get_nb_samples(p->data, p->len, RATE));
        S->stream_link = p->link;
        S->last_seq = -1;
    }
    S->last_pkt_ms = now;
    if (!decoder_ready(S)) return;
    if (S->last_seq >= 0) {
        int gap = (uint16_t)(p->seq - S->last_seq) - 1;
        if (gap < 0 || gap > 1000) return; /* late or repeated */
        M.lost += (uint32_t)gap;
        M.t_lost += (uint32_t)gap;
        int frame = opus_packet_get_nb_samples(p->data, p->len, RATE);
        for (int i = 0; i < gap && i < MAX_PLC && frame > 0; i++) {
            int n = opus_decode(S->dec, NULL, 0, pcm, frame, 0);
            if (n > 0) push(S, pcm, n);
        }
    }
    S->last_seq = p->seq;
    int n = opus_decode(S->dec, p->data, p->len, pcm, MAX_FRAME, 0);
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
    push(S, pcm, n);
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
    int16_t *out = port_alloc_bulk(CHUNK * sizeof(int16_t));
    if (!pp || !pcm || !out) {
        SLOGE(TAG, "no memory for the microphone decoder");
        free(pp);
        free(pcm);
        free(out);
        return;
    }
    pkt_t *p = pp;
    bool left = false;
    for (;;) {
        port_event_wait(M.ev, left ? WAIT_MS : 1000);
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
            left = mix(out);
        }
        left = mix(out);
        stats();
    }
}

int64_t mic_last_packet_ms(int slot)
{
    src_t *S = src(slot);
    if (!S) return 0;
    port_lock();
    int64_t t = S->last_rx_ms;
    port_unlock();
    return t;
}

void mic_set_talk(int slot, bool held)
{
    src_t *S = src(slot);
    if (!S) return;
    port_lock();
    bool was = S->talk_held;
    S->talk_held = held;
    if (was && !held) S->talk_released_ms = port_now_ms();
    port_unlock();
    if (g_cfg.mic_push_to_talk && was != held) {
        if (slot) {
            SLOGI(TAG, "push to talk, player %d: %s", slot + 1, held ? "talking" : "released");
        } else {
            SLOGI(TAG, "push to talk: %s", held ? "talking" : "released");
        }
    }
}

void stadia_mic_set_push_to_talk(bool on)
{
    port_lock();
    bool was = g_cfg.mic_push_to_talk;
    g_cfg.mic_push_to_talk = on;
    port_unlock();
    if (was != on) SLOGI(TAG, "push to talk %s", on ? "on" : "off");
}

void stadia_mic_set_gain(int headset_db, int builtin_db)
{
    if (headset_db < STADIA_MIC_GAIN_MIN) headset_db = STADIA_MIC_GAIN_MIN;
    if (headset_db > STADIA_MIC_GAIN_MAX) headset_db = STADIA_MIC_GAIN_MAX;
    if (builtin_db < STADIA_MIC_GAIN_MIN) builtin_db = STADIA_MIC_GAIN_MIN;
    if (builtin_db > STADIA_MIC_GAIN_MAX) builtin_db = STADIA_MIC_GAIN_MAX;
    M.gain_db[0] = (int8_t)headset_db;
    M.gain_db[1] = (int8_t)builtin_db;
}

void mic_set_headset(int slot, bool in)
{
    src_t *S = src(slot);
    if (!S) return;
    int8_t v = in ? 1 : 0;
    if (S->headset != v) {
        const char *what = in ? "headset in: its microphone" : "no headset: the built-in microphone";
        if (slot) {
            SLOGI(TAG, "player %d: %s", slot + 1, what);
        } else {
            SLOGI(TAG, "%s", what);
        }
    }
    S->headset = v;
}

bool stadia_mic_push_to_talk(void)
{
    port_lock();
    bool on = g_cfg.mic_push_to_talk;
    port_unlock();
    return on;
}

static bool talking(const src_t *S, int64_t now)
{
    return S->talk_held || (S->talk_released_ms && now - S->talk_released_ms < TALK_TAIL_MS);
}

bool mic_talking(int slot)
{
    src_t *S = src(slot);
    if (!S) return false;
    port_lock();
    bool on = talking(S, port_now_ms());
    port_unlock();
    return on;
}

static bool any_talking(void)
{
    int64_t now = port_now_ms();
    bool on = false;
    port_lock();
    for (int s = 0; s < g_nctl; s++) on |= talking(&M.S[s], now);
    port_unlock();
    return on;
}

bool mic_busy(void)
{
    if (!M.q) return false;
    int64_t now = port_now_ms();
    bool busy = false;
    port_lock();
    for (int s = 0; s < g_nctl; s++) {
        const src_t *S = &M.S[s];
        busy |= (g_cfg.mic_push_to_talk && S->talk_held) || (S->last_speech_ms && now - S->last_speech_ms < BUSY_MS);
    }
    port_unlock();
    return busy;
}

bool mic_init(void)
{
    for (int s = 0; s < STADIA_MAX_CONTROLLERS; s++) {
        M.S[s].last_seq = -1;
        M.S[s].headset = -1;
    }
    M.q = port_alloc_bulk(QUEUE_LEN * sizeof(pkt_t)); /* 18 KB of packets: PSRAM */
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
    int64_t last = 0;
    for (int s = 0; s < g_nctl; s++) {
        if (M.S[s].last_rx_ms > last) last = M.S[s].last_rx_ms;
    }
    port_unlock();
    st->mic_decoded = M.t_decoded;
    st->mic_lost = M.t_lost;
    st->mic_errors = M.t_errors;
    st->mic_peak = M.t_peak;
    M.t_peak = 0;
    st->mic_talking = any_talking();
    st->mic_headset = M.S[0].headset; /* player 1's */
    st->mic_gain_db = M.gain_db[M.S[0].headset == 0];
    int64_t age = port_now_ms() - last;
    st->mic_age_ms = !last ? -1 : age > INT32_MAX ? INT32_MAX : (int32_t)age;
}
