/*
 * The display link (the post-DTLS half of controller.py) and the task that runs both links.
 *
 *   RPCs     GetControllerStateRequest every 1.5 s from 0.6 s after DTLS until answered,
 *            then the controller's READY traffic: GetCurrentTime and keepalive requests
 *            answered, input notifications decoded
 *   game     StartGame 1 s after READY; the controller's ProxyRequest (InitializationRequest)
 *            answered with the game link's parameters (game.c)
 *   audio    SetAudioModeRequest(mode 2) at READY, then the headset frames as SRTP on the
 *            display link's pair (audio.c)
 *   mic      whatever the controller sends as SRTP media, to the microphone decoder (mic.c)
 *
 * Everything on the sockets runs in one task, so only the remote descriptions and the
 * callbacks need the lock.
 */
#include "game.h"
#include "audio.h"
#include "mic.h"
#include "rtc_link.h"
#include "stadia_internal.h"
#include "stadia_pb.h"
#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

static const char *TAG = "rtc";

enum { RPC_OK = 0, RPC_INVALID_ARGUMENT = 3, RPC_UNIMPLEMENTED = 12 };
/* ControllerToDisplay variants */
enum { C2D_PROXY_REQUEST = 1, C2D_CONTROLLER_STATE = 2, C2D_INPUT = 3, C2D_AUDIO_CONFIG = 4, C2D_DISCONNECT = 8,
       C2D_BATTERY = 10, C2D_KEEPALIVE = 14, C2D_GET_CURRENT_TIME = 18 };
/* DisplayToController fields */
enum { D2C_PROXY_RESPONSE = 1, D2C_GET_CONTROLLER_STATE = 2, D2C_START_GAME = 8, D2C_SET_AUDIO_MODE = 16,
       D2C_CURRENT_TIME = 21 };
enum { AUDIO_MODE_WEBRTC_OPUS = 2 };

#define DISPLAY_RPC_DELAY_MS 600
#define DISPLAY_RPC_INTERVAL_MS 1500
#define DISPLAY_RPC_WINDOW_MS (300 * 1000)
#define START_GAME_DELAY_MS 1000
#define AUDIO_ROUTE_WAIT_MS 1000 /* frames go out after the SetAudioMode answer, or this long after it */

typedef enum { DS_NONE, DS_DTLS, DS_ASKED, DS_STATE, DS_READY, DS_INPUT } display_state_t;
static const char *const DS_NAMES[] = {"none", "dtls-connected", "get-controller-state-sent",
                                       "controller-state-received", "ready", "input"};

static struct {
    rtc_link_t link;
    display_state_t state;
    uint32_t ticket;
    uint32_t state_ticket; /* our last GetControllerStateRequest */
    int64_t next_state_req;
    int state_reqs;
    /* StartGame */
    int64_t ready_at;
    bool start_sent;
    uint32_t start_ticket;
    int64_t start_at;
    char game_key[24];
    /* SetAudioMode */
    bool audio_sent, audio_answered;
    bool audio_f2; /* the request in flight asks for the microphone too */
    bool f2_refused; /* the controller refused that: ask without it from now on */
    uint32_t audio_ticket;
    int64_t audio_at;
} D;

/* ---- display RPCs ------------------------------------------------------------------- */

static void set_state(display_state_t s)
{
    if (s > D.state) {
        D.state = s;
        SLOGI(TAG, "display state: %s", DS_NAMES[s]);
        if (s >= DS_READY && !D.ready_at) D.ready_at = port_now_ms();
    }
}

static void send_rpc(int kind, uint32_t ticket, int code, const char *message, const uint8_t *payload, size_t plen)
{
    uint8_t out[1200];
    size_t n = rpc_encode(kind, ticket, code, message, payload, plen, out, sizeof(out));
    if (n) {
        rtc_link_send_wire(&D.link, WIRE_ID_RPC_DISPLAY, out, n);
    } else {
        SLOGE(TAG, "RPC does not fit (%u bytes of payload)", (unsigned)plen);
    }
}

static void ask_controller_state(void)
{
    uint8_t msg[8];
    pbw_t w = PBW(msg, sizeof(msg));
    uint8_t req[4];
    pbw_t r = PBW(req, sizeof(req));
    pbw_uint(&r, 2, 0); /* display_supports_send_log_report = false */
    pbw_bytes(&w, D2C_GET_CONTROLLER_STATE, r.buf, r.len);
    D.state_ticket = ++D.ticket;
    send_rpc(1, D.state_ticket, 0, NULL, w.buf, w.len);
    if (D.state_reqs++ < 3) SLOGI(TAG, "GetControllerStateRequest #%u sent", (unsigned)D.state_ticket);
    set_state(DS_ASKED);
}

/* DisplayToController #8 StartGame {1: key (max 23), 2: 1}. The controller holds the reply
 * until its game session connects (OK) or fails. */
static void send_start_game(void)
{
    uint8_t rnd[8];
    port_random(rnd, sizeof(rnd));
    snprintf(D.game_key, sizeof(D.game_key), "local-%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3],
             rnd[4], rnd[5], rnd[6], rnd[7]);
    uint8_t sg[40], msg[48];
    pbw_t s = PBW(sg, sizeof(sg));
    pbw_str(&s, 1, D.game_key);
    pbw_uint(&s, 2, 1);
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, D2C_START_GAME, s.buf, s.len);
    D.start_ticket = ++D.ticket;
    D.start_at = port_now_ms();
    D.start_sent = true;
    send_rpc(1, D.start_ticket, 0, NULL, m.buf, m.len);
    SLOGI(TAG, "StartGame %s sent", D.game_key);
}

/* DisplayToController #16 SetAudioModeRequest {1 mode: 2, 3 f3: true}: the WebRTC Opus route to
 * the headset jack. Without it the controller takes the RTP and plays nothing.
 *
 * With g_cfg.mic_request it also sets f2, the request's one other (bool) field. Its meaning is
 * not known; it is our best guess for "and send me the microphone", next to f3 for the
 * headphones. If the controller refuses the request with f2, it is asked again without. */
static void send_set_audio_mode(void)
{
    uint8_t am[8], msg[16];
    pbw_t a = PBW(am, sizeof(am));
    pbw_uint(&a, 1, AUDIO_MODE_WEBRTC_OPUS);
    D.audio_f2 = g_cfg.mic_request && !D.f2_refused;
    if (D.audio_f2) pbw_uint(&a, 2, 1);
    pbw_uint(&a, 3, 1);
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, D2C_SET_AUDIO_MODE, a.buf, a.len);
    D.audio_ticket = ++D.ticket;
    D.audio_at = port_now_ms();
    D.audio_sent = true;
    D.audio_answered = false;
    send_rpc(1, D.audio_ticket, 0, NULL, m.buf, m.len);
    SLOGI(TAG, "SetAudioMode(WebRTC Opus%s) sent", D.audio_f2 ? ", f2 = microphone?" : "");
}

static int32_t zigzag(uint64_t v)
{
    return (int32_t)((v >> 1) ^ (~(v & 1) + 1));
}

static int16_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

/* ControllerToDisplay #3: f1 buttons, f2-f5 sint32 sticks, f6/f7 triggers. */
static void on_input(const uint8_t *m, size_t len)
{
    stadia_pad_t pad = {.connected = true};
    uint32_t lt = 0, rt = 0;
    pbf_t f;
    pbr_t r = PBR(m, len);
    while (pbr_next(&r, &f)) {
        switch (f.field) {
        case 1: pad.buttons = (uint32_t)f.v; break;
        case 2: pad.lx = clamp16(zigzag(f.v)); break;
        case 3: pad.ly = clamp16(zigzag(f.v)); break;
        case 4: pad.rx = clamp16(zigzag(f.v)); break;
        case 5: pad.ry = clamp16(zigzag(f.v)); break;
        case 6: lt = (uint32_t)f.v; break;
        case 7: rt = (uint32_t)f.v; break;
        }
    }
    pad.lt = lt > 255 ? 255 : (uint8_t)lt;
    pad.rt = rt > 255 ? 255 : (uint8_t)rt;
    set_state(DS_INPUT);
    pad_report(&pad, "rtc");
}

/* ControllerToDisplay #1 ProxyRequest {1 f1, 2 key, 4 body: an inner RpcMessage request whose
 * payload is a StreamerRequest}. Answered with DisplayToController #1 ProxyResponse {1 data:
 * the inner RpcMessage response carrying a StreamerResponse}. */
static void on_proxy_request(uint32_t ticket, const uint8_t *pr, size_t n)
{
    pbf_t body, init;
    rpc_msg_t inner;
    if (!pb_find(pr, n, 4, &body) || !rpc_decode(body.data, body.len, &inner) || inner.kind != 1 ||
        !inner.has_ticket || !inner.payload) {
        SLOGW(TAG, "ProxyRequest with an unparsable body");
        send_rpc(2, ticket, RPC_INVALID_ARGUMENT, "Unparsable proxied StreamerRequest", NULL, 0);
        return;
    }
    uint8_t sresp[800], inner_resp[900];
    size_t ilen;
    if (!pb_find(inner.payload, inner.plen, 1, &init) || init.wt != PB_LEN) {
        SLOGI(TAG, "proxied StreamerRequest of an unknown kind: UNIMPLEMENTED");
        ilen = rpc_encode(2, inner.ticket, RPC_UNIMPLEMENTED, "Unimplemented StreamerRequest in local receiver", NULL, 0,
                          inner_resp, sizeof(inner_resp));
    } else {
        size_t slen = game_initialization(init.data, init.len, sresp, sizeof(sresp));
        ilen = slen ? rpc_encode(2, inner.ticket, RPC_OK, "", sresp, slen, inner_resp, sizeof(inner_resp))
                    : rpc_encode(2, inner.ticket, RPC_INVALID_ARGUMENT, "Undecodable RtcSdpOffer", NULL, 0, inner_resp,
                                 sizeof(inner_resp));
    }
    uint8_t prsp[920], msg[940];
    pbw_t p = PBW(prsp, sizeof(prsp));
    pbw_bytes(&p, 1, inner_resp, ilen);
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, D2C_PROXY_RESPONSE, p.buf, p.len);
    if (!ilen || p.overflow || m.overflow) {
        SLOGE(TAG, "ProxyResponse does not fit");
        return;
    }
    send_rpc(2, ticket, RPC_OK, "", m.buf, m.len);
}

static void on_rpc(const uint8_t *data, size_t len)
{
    rpc_msg_t rpc;
    if (!rpc_decode(data, len, &rpc)) {
        SLOGW(TAG, "undecodable RpcMessage (%u bytes)", (unsigned)len);
        return;
    }
    pbf_t variant = {0}, f;
    if (rpc.payload) {
        pbr_t pr = PBR(rpc.payload, rpc.plen);
        pbr_next(&pr, &variant);
    }

    if (rpc.kind == 1) { /* the controller's requests */
        if (!rpc.has_ticket) return;
        switch (variant.field) {
        case C2D_GET_CURRENT_TIME: {
            set_state(DS_READY);
            pbf_t t;
            int64_t ctl_us = pb_find(variant.data, variant.len, 1, &t) ? (int64_t)t.v : 0;
            uint8_t ct[32], msg[40];
            pbw_t w = PBW(ct, sizeof(ct));
            pbw_sint64(&w, 1, ctl_us); /* echoed unchanged: the firmware CHECK-fails on rtt < 0 */
            pbw_sint64(&w, 2, (int64_t)(port_wall_time() * 1e6));
            pbw_t m = PBW(msg, sizeof(msg));
            pbw_bytes(&m, D2C_CURRENT_TIME, w.buf, w.len);
            send_rpc(2, rpc.ticket, RPC_OK, "", m.buf, m.len);
            break;
        }
        case C2D_KEEPALIVE:
            set_state(DS_READY);
            send_rpc(2, rpc.ticket, RPC_OK, "", NULL, 0);
            break;
        case C2D_DISCONNECT:
            SLOGI(TAG, "the controller asks to disconnect");
            send_rpc(2, rpc.ticket, RPC_OK, "", NULL, 0);
            break;
        case C2D_PROXY_REQUEST:
            on_proxy_request(rpc.ticket, variant.data, variant.len);
            break;
        default:
            SLOGI(TAG, "request #%u (ControllerToDisplay #%u): answered UNIMPLEMENTED", (unsigned)rpc.ticket,
                  (unsigned)variant.field);
            send_rpc(2, rpc.ticket, RPC_UNIMPLEMENTED, "Unimplemented RPC in local receiver", NULL, 0);
        }
    } else if (rpc.kind == 3) { /* notifications */
        if (variant.field == C2D_INPUT) {
            on_input(variant.data, variant.len);
        } else if (variant.field == C2D_BATTERY) {
            pbf_t v;
            if (pb_find(variant.data, variant.len, 1, &v)) {
                uint32_t bits = (uint32_t)v.v;
                float level;
                memcpy(&level, &bits, 4);
                SLOGI(TAG, "battery %d%%", (int)(level * 100 + 0.5f));
            }
        } else if (variant.field == C2D_AUDIO_CONFIG) {
            pbf_t mode;
            bool in = pb_find(variant.data, variant.len, 1, &mode) && mode.v == 1;
            SLOGI(TAG, "headset %s", in ? "plugged in" : "out");
        } else {
            SLOGI(TAG, "notification ControllerToDisplay #%u", (unsigned)variant.field);
        }
    } else if (rpc.kind == 2 && D.audio_sent && rpc.ticket == D.audio_ticket) {
        /* a repeat in the same boot is FAILED_PRECONDITION ("already in WebRTC Opus mode"): open too */
        SLOGI(TAG, "SetAudioMode%s answered: code %u%s", D.audio_f2 ? " with f2" : "", (unsigned)rpc.code,
              rpc.code == 9 ? " (already open)" : "");
        if (rpc.code != 0 && rpc.code != 9 && D.audio_f2) {
            SLOGW(TAG, "the controller refuses SetAudioMode with f2: asking without it");
            D.f2_refused = true;
            send_set_audio_mode();
            return;
        }
        D.audio_answered = true;
    } else if (rpc.kind == 2 && D.start_sent && rpc.ticket == D.start_ticket) {
        char msg[96] = "";
        pbf_t body;
        pbr_t r = PBR(data, len);
        if (pbr_next(&r, &body)) {
            pbr_t b = PBR(body.data, body.len);
            while (pbr_next(&b, &f)) {
                if (f.field == 3) pbf_str(&f, msg, sizeof(msg));
            }
        }
        int64_t after = port_now_ms() - D.start_at;
        if (rpc.code == 0) {
            SLOGI(TAG, "StartGame answered OK after %d ms: the game session is up", (int)after);
        } else {
            SLOGW(TAG, "StartGame failed after %d ms: code %u %s", (int)after, (unsigned)rpc.code, msg);
        }
    } else if (rpc.kind == 2 && rpc.ticket == D.state_ticket && D.state < DS_STATE) {
        if (rpc.code != 0 || variant.field != C2D_CONTROLLER_STATE) {
            if (D.state_reqs <= 3) {
                SLOGI(TAG, "GetControllerState answered with code %u, asking again", (unsigned)rpc.code);
            }
            return;
        }
        char product[24] = "", hw[16] = "", fw[16] = "";
        bool headset = false;
        pbr_t r = PBR(variant.data, variant.len);
        while (pbr_next(&r, &f)) {
            if (f.field == 4) headset = f.v != 0;
            if (f.field == 14) pbf_str(&f, product, sizeof(product));
            if (f.field == 15) pbf_str(&f, hw, sizeof(hw));
            if (f.field == 16) pbf_str(&f, fw, sizeof(fw));
        }
        SLOGI(TAG, "controller: %s, hardware %s, firmware %s, headset %s", product, hw, fw, headset ? "in" : "out");
        set_state(DS_STATE);
    }
}

/* ---- display link callbacks --------------------------------------------------------- */

/* Input as RTP on sub-channel 0x66 (the fake controller's display-link input): acked with
 * InputProtocolMessage{ack_sequence_number} the same way. */
static void on_wire_rtp(const uint8_t *p, size_t len)
{
    uint8_t pt;
    const uint8_t *pl;
    size_t pn;
    pbf_t series, seq;
    if (!rtp_payload(p, len, &pt, &pl, &pn) || !pb_find(pl, pn, 7, &series) ||
        !pb_find(series.data, series.len, 2, &seq)) {
        return;
    }
    set_state(DS_INPUT);
    SLOGI(TAG, "input series #%llu on 0x66 (%u bytes)", (unsigned long long)seq.v, (unsigned)pn);
    uint8_t ack[16];
    pbw_t a = PBW(ack, sizeof(ack));
    pbw_uint(&a, 4, seq.v);
    rtc_link_send_rtp_wire(&D.link, pt, a.buf, a.len);
}

static void display_on_wire(rtc_link_t *L, uint16_t id, const uint8_t *p, size_t n)
{
    if (id == WIRE_ID_RTP) {
        on_wire_rtp(p, n);
    } else if (id == WIRE_ID_RPC_CONTROLLER || id == WIRE_ID_RPC_DISPLAY) {
        on_rpc(p, n);
    } else {
        SLOGI(TAG, "WireHeader2 id 0x%04x, %u bytes", id, (unsigned)n);
    }
}

static void display_on_connected(rtc_link_t *L)
{
    D.state = DS_DTLS;
    D.next_state_req = L->dtls_at + DISPLAY_RPC_DELAY_MS;
    D.state_reqs = 0;
    D.ready_at = 0;
    D.start_sent = false;
    D.audio_sent = false;
    audio_set_route(false);
}

static void display_on_closed(rtc_link_t *L)
{
    D.state = DS_NONE;
    audio_set_route(false);
    stadia_pad_t released = {.connected = false};
    pad_report(&released, "rtc");
}

static void display_on_media(rtc_link_t *L, const uint8_t *rtp, size_t n)
{
    mic_on_rtp(L, 0, rtp, n);
}

static const rtc_link_ops_t DISPLAY_OPS = {
    .on_connected = display_on_connected,
    .on_closed = display_on_closed,
    .on_wire = display_on_wire,
    .on_media = display_on_media,
};

/* ---- the task ----------------------------------------------------------------------- */

static void display_tick(void)
{
    rtc_link_tick(&D.link);
    int64_t now = port_now_ms();
    /* GetControllerStateRequest until answered */
    if (rtc_link_up(&D.link) && D.state < DS_STATE && now >= D.next_state_req &&
        now - D.link.dtls_at < DISPLAY_RPC_WINDOW_MS) {
        ask_controller_state();
        D.next_state_req = now + DISPLAY_RPC_INTERVAL_MS;
    }
    /* StartGame once per connection, a second after READY */
    if (g_cfg.start_game && rtc_link_up(&D.link) && !D.start_sent && D.ready_at &&
        now - D.ready_at >= START_GAME_DELAY_MS) {
        send_start_game();
    }
    /* the headset audio route: asked for at READY (before StartGame, as stadiacast does) */
    if (g_cfg.audio && rtc_link_up(&D.link) && D.ready_at && !D.audio_sent) send_set_audio_mode();
    if (D.audio_sent && rtc_link_up(&D.link) && (D.audio_answered || now - D.audio_at >= AUDIO_ROUTE_WAIT_MS)) {
        audio_set_route(true);
    }
    audio_tick(&D.link);
}

static void poll_socket(rtc_link_t *L, uint8_t *buf, size_t cap)
{
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int n = recvfrom(L->sock, buf, cap, 0, (struct sockaddr *)&from, &flen);
    if (n > 0) rtc_link_poll(L, buf, (size_t)n, &from);
}

static void rtc_task(void *arg)
{
    /* From the heap: .bss on v1.x P4 chips comes out of the small RAM pool the system starts
     * from (see c6_update.c), and the microphone's additions had used up what was left. */
    const size_t cap = 1600;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        SLOGE(TAG, "no memory for the rtc task");
        return;
    }
    rtc_link_t *G = game_link();
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(D.link.sock, &fds);
        FD_SET(G->sock, &fds);
        int maxfd = D.link.sock > G->sock ? D.link.sock : G->sock;
        struct timeval tv = {.tv_sec = 0, .tv_usec = audio_poll_ms() * 1000};
        int r = select(maxfd + 1, &fds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(D.link.sock, &fds)) poll_socket(&D.link, buf, cap);
        if (r > 0 && FD_ISSET(G->sock, &fds)) poll_socket(G, buf, cap);
        display_tick();
        game_tick();
    }
}

void rtc_set_remote(const remote_offer_t *remote)
{
    game_reset();
    rtc_link_set_remote(&D.link, remote);
}

bool rtc_start(uint16_t display_port, uint16_t game_port)
{
    if (!rtc_link_global_init()) return false;
    audio_init(&g_cfg);
    if (g_cfg.mic) mic_init();
    if (!rtc_link_init(&D.link, "display", display_port, &DISPLAY_OPS)) return false;
    const rtc_identity_t *id = rtc_identity();
    rtc_link_set_credentials(&D.link, id->ufrag, id->pwd);
    if (!game_init(game_port)) return false;
    return port_spawn(rtc_task, NULL, "rtc", 20 * 1024);
}
