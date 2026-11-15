/*
 * The game session (port of game_session.py and input_protocol.py).
 *
 * After StartGame the controller's StreamingClient sends an InitializationRequest through the
 * display link (rtc.c answers it with game_initialization()), then connects to this second
 * link as the DTLS client. Input arrives on sub-channel 0x66 as InputProtocolMessage (in RTP,
 * bare, or as a bare DTLS record) and is acked the same way. The same channel is the only
 * path to the rumble motors: InputFeedbackMessage_V2.gamepad_vibration, absolute levels
 * that the controller's haptics watchdog stops 5 s after the last write, so a held level is
 * re-sent every 2 s.
 */
#include "game.h"
#include "mic.h"

#include "stadia_pb.h"
#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "game";

#define GAME_ICE_UFRAG_LEN 6 /* the controller's decoder: ufrag 7, password 31 at most */
#define GAME_ICE_PWD_LEN 24
#define RUMBLE_REFRESH_MS 2000
#define INPUT_PT_DEFAULT 96
#define STICK_LOG_MS 500

typedef enum { FRAMING_NONE, FRAMING_RTP, FRAMING_RAW, FRAMING_UNFRAMED } framing_t;
static const char *const FRAMING_NAMES[] = {"none", "rtp", "raw", "unframed"};

static struct {
    rtc_link_t link;
    framing_t framing;
    uint8_t input_pt;
    char series_session[64];
    uint32_t gamepad_id;
    uint32_t input_messages, input_errors;
    uint64_t feedback_seq;
    int rpc_logged, other_logged;
    /* rumble: wanted (set from any task under the lock) and what the controller has */
    uint16_t want_l, want_r;
    uint16_t sent_l, sent_r;
    bool sent_valid;
    int64_t sent_at;
    uint32_t rumbles;
    bool reset_pending;
} G;

static struct {
    stadia_pad_cb_t cb;
    void *arg;
    stadia_pad_t last;
    bool have_last;
    int64_t stick_log_at;
} P;

/* ---- the gamepad callback ------------------------------------------------------------ */

void stadia_set_pad_callback(stadia_pad_cb_t cb, void *arg)
{
    port_lock();
    P.cb = cb;
    P.arg = arg;
    port_unlock();
}

void pad_report(const stadia_pad_t *pad, const char *via)
{
    static const char *const NAMES[] = {"Up", "Down", "Left", "Right", "Menu", "Options", "L3", "R3", "L1",
                                        "R1", "Assistant", "Capture", "A", "B", "X", "Y", "Stadia"};
    uint32_t changed = P.have_last ? pad->buttons ^ P.last.buttons : pad->buttons;
    bool moved = !P.have_last || pad->lx != P.last.lx || pad->ly != P.last.ly || pad->rx != P.last.rx ||
                 pad->ry != P.last.ry || pad->lt != P.last.lt || pad->rt != P.last.rt;
    char line[200] = "";
    size_t n = 0;
    for (int b = 0; b < 17; b++) {
        if (changed & (1u << b)) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %s%s", NAMES[b], (pad->buttons & (1u << b)) ? "+" : "-");
        }
    }
    /* stick and trigger reports come in fast: log those at most two a second */
    int64_t now = port_now_ms();
    if (n || (moved && now >= P.stick_log_at)) {
        SLOGI(via, "input:%s L(%d,%d) R(%d,%d) L2 %u R2 %u", n ? line : " (sticks)", pad->lx, pad->ly, pad->rx,
              pad->ry, pad->lt, pad->rt);
        if (!n) P.stick_log_at = now + STICK_LOG_MS;
    }
    P.last = *pad;
    P.have_last = pad->connected;
    port_lock();
    stadia_pad_cb_t cb = P.cb;
    void *arg = P.arg;
    port_unlock();
    if (cb) cb(pad, arg);
}

/* ---- rumble ------------------------------------------------------------------------- */

void stadia_rumble(uint16_t left, uint16_t right)
{
    port_lock();
    G.want_l = left;
    G.want_r = right;
    port_unlock();
}

static bool send_input_message(const uint8_t *msg, size_t len)
{
    rtc_link_t *L = &G.link;
    switch (G.framing) {
    case FRAMING_RAW: return rtc_link_send_wire(L, WIRE_ID_RTP, msg, len);
    case FRAMING_UNFRAMED: return rtc_link_send_record(L, msg, len);
    default: return rtc_link_send_rtp_wire(L, G.input_pt, msg, len);
    }
}

/* InputProtocolMessage{7: series{1 session_id, 2 seq, 3 base_timestamp_us, 4: slim{4: 0,
 * 10: feedback{1: vibration{1 id, 2 left, 3 right}}}}} */
static bool send_vibration(uint16_t left, uint16_t right)
{
    uint8_t vib[24], fb[32], slim[40], series[160], msg[170];
    pbw_t v = PBW(vib, sizeof(vib));
    pbw_uint(&v, 1, G.gamepad_id);
    pbw_uint(&v, 2, left);
    pbw_uint(&v, 3, right);
    pbw_t f = PBW(fb, sizeof(fb));
    pbw_bytes(&f, 1, v.buf, v.len);
    pbw_t s = PBW(slim, sizeof(slim));
    pbw_uint(&s, 4, 0);
    pbw_bytes(&s, 10, f.buf, f.len);
    pbw_t se = PBW(series, sizeof(series));
    if (G.series_session[0]) pbw_str(&se, 1, G.series_session);
    pbw_uint(&se, 2, ++G.feedback_seq);
    pbw_uint(&se, 3, (uint64_t)(port_wall_time() * 1e6));
    pbw_bytes(&se, 4, s.buf, s.len);
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, 7, se.buf, se.len);
    if (m.overflow || se.overflow || !send_input_message(m.buf, m.len)) {
        G.feedback_seq--;
        return false;
    }
    return true;
}

static void rumble_tick(void)
{
    if (!rtc_link_up(&G.link)) return;
    port_lock();
    uint16_t l = G.want_l, r = G.want_r;
    port_unlock();
    int64_t now = port_now_ms();
    bool changed = !G.sent_valid ? (l || r) : (l != G.sent_l || r != G.sent_r);
    bool refresh = G.sent_valid && (l || r) && now - G.sent_at >= RUMBLE_REFRESH_MS;
    if (!changed && !refresh) return;
    if (!send_vibration(l, r)) return;
    if (changed && G.rumbles++ < 20) SLOGI(TAG, "rumble left %u right %u", l, r);
    G.sent_l = l, G.sent_r = r, G.sent_valid = true, G.sent_at = now;
}

/* ---- input -------------------------------------------------------------------------- */

static int32_t zigzag(uint64_t v)
{
    return (int32_t)((v >> 1) ^ (~(v & 1) + 1));
}

static int16_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* GamepadMessage_V2 {1 id, 2 buttons, 3-6 sint32 sticks, 7-8 triggers} */
static void on_gamepad_v2(const uint8_t *m, size_t len)
{
    stadia_pad_t pad = {.connected = true};
    int32_t ax[4] = {0};
    uint32_t lt = 0, rt = 0;
    pbf_t f;
    pbr_t r = PBR(m, len);
    while (pbr_next(&r, &f)) {
        switch (f.field) {
        case 1: G.gamepad_id = (uint32_t)f.v; break;
        case 2: pad.buttons = (uint32_t)f.v; break;
        case 3: case 4: case 5: case 6: ax[f.field - 3] = zigzag(f.v); break;
        case 7: lt = (uint32_t)f.v; break;
        case 8: rt = (uint32_t)f.v; break;
        }
    }
    pad.lx = clamp16(ax[0]), pad.ly = clamp16(ax[1]), pad.rx = clamp16(ax[2]), pad.ry = clamp16(ax[3]);
    pad.lt = lt > 255 ? 255 : (uint8_t)lt;
    pad.rt = rt > 255 ? 255 : (uint8_t)rt;
    pad_report(&pad, "game");
}

/* InputMessageSeries {1 session_id, 2 sequence_number, 4 repeated InputMessageSlim}; returns
 * the sequence number (0 = none). */
static uint64_t on_series(const uint8_t *s, size_t len)
{
    uint64_t seq = 0;
    pbf_t f, g;
    pbr_t r = PBR(s, len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) pbf_str(&f, G.series_session, sizeof(G.series_session));
        if (f.field == 2) seq = f.v;
        if (f.field == 4 && f.wt == PB_LEN && pb_find(f.data, f.len, 5, &g) && g.wt == PB_LEN) {
            on_gamepad_v2(g.data, g.len);
        }
    }
    return seq;
}

static void on_input(const uint8_t *p, size_t len, framing_t framing, uint8_t pt)
{
    pbf_t series, comp;
    uint64_t seq = 0;
    if (pb_find(p, len, 7, &series) && series.wt == PB_LEN) {
        seq = on_series(series.data, series.len);
    } else if (pb_find(p, len, 5, &comp) && comp.wt == PB_LEN) {
        uint8_t *raw = malloc(4096);
        size_t n = raw ? port_inflate(comp.data, comp.len, raw, 4096) : 0;
        if (n) seq = on_series(raw, n);
        free(raw);
        if (!n) {
            if (G.input_errors++ < 5) SLOGW(TAG, "compressed input series that does not inflate (%u bytes)", (unsigned)comp.len);
            return;
        }
    } else {
        return; /* an ack of our feedback, or empty */
    }
    if (G.framing != framing && !(framing == FRAMING_UNFRAMED && G.framing != FRAMING_NONE)) {
        SLOGI(TAG, "input framing: %s", FRAMING_NAMES[framing]);
        G.framing = framing;
    }
    if (framing == FRAMING_RTP) G.input_pt = pt;
    if (G.input_messages++ == 0) SLOGI(TAG, "first input on the game session");
    if (seq) {
        uint8_t ack[16];
        pbw_t a = PBW(ack, sizeof(ack));
        pbw_uint(&a, 4, seq);
        send_input_message(a.buf, a.len);
    }
}

/* ---- link callbacks ----------------------------------------------------------------- */

static void on_wire(rtc_link_t *L, uint16_t id, const uint8_t *p, size_t n)
{
    if (id == WIRE_ID_RTP) {
        uint8_t pt;
        const uint8_t *pl;
        size_t pn;
        if (rtp_payload(p, n, &pt, &pl, &pn)) {
            on_input(pl, pn, FRAMING_RTP, pt);
        } else if (!(n >= 2 && (p[0] >> 6) == 2 && p[1] >= 192 && p[1] <= 223)) { /* RTCP is dropped */
            on_input(p, n, FRAMING_RAW, 0);
        }
    } else if (id == WIRE_ID_RPC || id == WIRE_ID_RPC_CONTROLLER || id == WIRE_ID_RPC_DISPLAY) {
        /* the StreamingClient's own RPCs: an empty OK, as --game-rpc-reply ok */
        rpc_msg_t m;
        if (!rpc_decode(p, n, &m)) return;
        if (G.rpc_logged++ < 3) {
            SLOGI(TAG, "RPC on 0x%04x: kind %d ticket %u, %u bytes", id, m.kind, (unsigned)m.ticket, (unsigned)m.plen);
        }
        if (m.kind != 1 || !m.has_ticket) return;
        uint8_t out[64];
        size_t len = rpc_encode(2, m.ticket, 0, "", NULL, 0, out, sizeof(out));
        uint16_t reply_id = id == WIRE_ID_RPC_CONTROLLER ? WIRE_ID_RPC_DISPLAY : id;
        if (len) rtc_link_send_wire(L, reply_id, out, len);
    } else if (G.other_logged++ < 3) {
        SLOGI(TAG, "WireHeader2 id 0x%04x, %u bytes", id, (unsigned)n);
    }
}

static void on_unframed(rtc_link_t *L, const uint8_t *p, size_t n)
{
    on_input(p, n, FRAMING_UNFRAMED, 0);
}

static void on_connected(rtc_link_t *L)
{
    SLOGI(TAG, "game session connected");
    G.sent_valid = false; /* re-send the wanted rumble level on the new session */
}

static void on_closed(rtc_link_t *L)
{
    SLOGI(TAG, "game session closed after %u input messages", (unsigned)G.input_messages);
}

static void on_media(rtc_link_t *L, const uint8_t *rtp, size_t n)
{
    mic_on_rtp(L, 1, rtp, n);
}

static const rtc_link_ops_t OPS = {
    .on_media = on_media,
    .on_connected = on_connected,
    .on_closed = on_closed,
    .on_wire = on_wire,
    .on_unframed = on_unframed,
};

/* ---- signaling through the display link ---------------------------------------------- */

size_t game_initialization(const uint8_t *init, size_t n, uint8_t *out, size_t cap)
{
    pbf_t f, offer = {0};
    int32_t version = -1;
    pbr_t r = PBR(init, n);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) version = (int32_t)f.v;
        if (f.field == 2 && f.wt == PB_LEN) offer = f;
    }
    remote_offer_t remote;
    memset(&remote, 0, sizeof(remote));
    offer_echo_t echo;
    if (!offer.data || !signaling_parse_offer(offer.data, offer.len, &remote, &echo)) {
        SLOGW(TAG, "InitializationRequest without a usable offer");
        return 0;
    }
    char ufrag[GAME_ICE_UFRAG_LEN + 1], pwd[GAME_ICE_PWD_LEN + 1];
    rtc_ice_string(ufrag, GAME_ICE_UFRAG_LEN);
    rtc_ice_string(pwd, GAME_ICE_PWD_LEN);
    rtc_link_set_credentials(&G.link, ufrag, pwd);
    rtc_link_set_remote(&G.link, &remote);

    uint8_t answer[600], cand[96];
    size_t alen = signaling_build_answer(&echo, ufrag, pwd, answer, sizeof(answer));
    size_t clen = signaling_build_candidate(G.link.port, cand, sizeof(cand));
    uint8_t ir[720];
    pbw_t i = PBW(ir, sizeof(ir)); /* InitializationResponse {1 answer, 2 candidate} */
    pbw_bytes(&i, 1, answer, alen);
    pbw_bytes(&i, 2, cand, clen);
    pbw_t o = PBW(out, cap); /* StreamerResponse {1 initialization} */
    pbw_bytes(&o, 1, i.buf, i.len);
    if (!alen || !clen || i.overflow || o.overflow) {
        SLOGE(TAG, "InitializationResponse does not fit");
        return 0;
    }
    G.framing = FRAMING_NONE;
    G.input_pt = INPUT_PT_DEFAULT;
    G.input_messages = 0;
    SLOGI(TAG, "InitializationRequest v%d (raw_dtls=%d): answering with %s:%u", (int)version, echo.raw_dtls,
          g_cfg.advertise_ip, (unsigned)G.link.port);
    return o.len;
}

/* ---- lifecycle ---------------------------------------------------------------------- */

rtc_link_t *game_link(void)
{
    return &G.link;
}

void game_reset(void)
{
    port_lock();
    G.reset_pending = true;
    port_unlock();
}

static void apply_reset(void)
{
    port_lock();
    bool reset = G.reset_pending;
    G.reset_pending = false;
    port_unlock();
    if (!reset) return;
    G.series_session[0] = '\0';
    G.gamepad_id = 0;
    G.feedback_seq = 0;
    G.sent_valid = false;
    P.have_last = false;
}

void game_tick(void)
{
    apply_reset();
    rtc_link_tick(&G.link);
    rumble_tick();
}

bool game_init(uint16_t port)
{
    G.input_pt = INPUT_PT_DEFAULT;
    return rtc_link_init(&G.link, "game", port, &OPS);
}
