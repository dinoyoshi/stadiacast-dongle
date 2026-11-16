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
 *   mic      DisplayToController #5 (true) after the audio route, repeated while the stream is
 *            quiet; what the controller sends as SRTP media goes to the decoder (mic.c)
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
enum { D2C_PROXY_RESPONSE = 1, D2C_GET_CONTROLLER_STATE = 2, D2C_MIC_ON = 5, D2C_START_GAME = 8,
       D2C_SET_AUDIO_MODE = 16, D2C_CURRENT_TIME = 21 };
enum { AUDIO_MODE_WEBRTC_OPUS = 2 };

#define DISPLAY_RPC_DELAY_MS 600
#define DISPLAY_RPC_INTERVAL_MS 1500
#define DISPLAY_RPC_WINDOW_MS (300 * 1000)
#define START_GAME_DELAY_MS 1000
#define AUDIO_ROUTE_WAIT_MS 1000 /* frames go out after the SetAudioMode answer, or this long after it */

typedef enum { DS_NONE, DS_DTLS, DS_ASKED, DS_STATE, DS_READY, DS_INPUT } display_state_t;
static const char *const DS_NAMES[] = {"none", "dtls-connected", "get-controller-state-sent",
                                       "controller-state-received", "ready", "input"};

typedef struct {
    rtc_link_t link;
    char name[20];
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
    uint32_t audio_ticket;
    int64_t audio_at;
    /* the battery gauge: kept across sessions (a new one reports again only minutes in) */
    int battery_pct; /* -1: no reading yet */
    int64_t battery_at;
} disp_t;

/* One per slot (player); slot 0's in internal RAM, the others' in PSRAM. */
static disp_t *DS[STADIA_MAX_CONTROLLERS];
/* Log tag: "rtc" for player 1 as before, "display2" ... for the others. */
#define DTAG(D) ((D)->link.slot ? (D)->name : TAG)

static void send_rpc(disp_t *D, int kind, uint32_t ticket, int code, const char *message, const uint8_t *payload,
                     size_t plen);

/* ---- the microphone ---------------------------------------------------------------- */

/* DisplayToController #5 (a BoolValue) with true makes the controller send its microphone: Opus,
 * PT 111, 20 ms frames with DTX (3-byte frames in silence), as SRTP on the display link's pair
 * under the display offer's audio SSRC. Found on a controller on firmware 316382 by sending each
 * request whose use was not known: it answered OK and the stream began 24 ms later. The stream
 * also stopped by itself after a while, so the request is repeated whenever nothing has come for
 * MIC_QUIET_MS (it is answered OK while the stream runs too).
 *
 * With push to talk the request goes out when the Assistant button is pressed (the stream starts
 * within a frame or two), is repeated sooner (MIC_PTT_QUIET_MS) while it is held, and #5 false
 * goes out once it is let go; mic.c drops whatever still comes while nobody talks. */
#define MIC_ON_DELAY_MS 500 /* after the SetAudioMode answer */
#define MIC_QUIET_MS 3000
#define MIC_PTT_QUIET_MS 1000 /* DTX still sends a frame every 400 ms or so in silence */

static struct {
    int64_t ready_at; /* 0: not yet due */
    int64_t sent_at;
    uint32_t ticket;
    uint32_t sent;
    bool on; /* the last request was true */
} MIC;

static void send_mic(disp_t *D, bool on, int64_t now)
{
    uint8_t v[4], msg[8];
    pbw_t b = PBW(v, sizeof(v));
    if (on) pbw_uint(&b, 1, 1); /* false is the empty BoolValue */
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, D2C_MIC_ON, b.buf, b.len);
    MIC.ticket = ++D->ticket;
    send_rpc(D, 1, MIC.ticket, 0, NULL, m.buf, m.len);
    if (g_cfg.mic_push_to_talk || MIC.sent < 3) {
        SLOGI(DTAG(D), "microphone %s (DisplayToController #5 %s) sent", on ? "on" : "off", on ? "true" : "false");
    }
    MIC.sent++;
    MIC.sent_at = now;
    MIC.on = on;
}

static void mic_tick(disp_t *D, int64_t now)
{
    if (!g_cfg.mic || D->link.slot != 0 || !rtc_link_up(&D->link) || !D->audio_answered) return;
    if (!MIC.ready_at) MIC.ready_at = now + MIC_ON_DELAY_MS;
    if (now < MIC.ready_at) return;
    bool ptt = g_cfg.mic_push_to_talk;
    if (ptt && !mic_talking()) {
        if (MIC.on) send_mic(D, false, now);
        return;
    }
    int64_t quiet = ptt ? MIC_PTT_QUIET_MS : MIC_QUIET_MS;
    if (!MIC.on || (now - MIC.sent_at >= quiet && now - mic_last_packet_ms() >= quiet)) send_mic(D, true, now);
}

/* ---- display RPCs ------------------------------------------------------------------- */

static void set_state(disp_t *D, display_state_t s)
{
    if (s > D->state) {
        D->state = s;
        SLOGI(DTAG(D), "display state: %s", DS_NAMES[s]);
        if (s >= DS_READY && !D->ready_at) D->ready_at = port_now_ms();
    }
}

static void send_rpc(disp_t *D, int kind, uint32_t ticket, int code, const char *message, const uint8_t *payload,
                     size_t plen)
{
    uint8_t out[1200];
    size_t n = rpc_encode(kind, ticket, code, message, payload, plen, out, sizeof(out));
    if (n) {
        rtc_link_send_wire(&D->link, WIRE_ID_RPC_DISPLAY, out, n);
    } else {
        SLOGE(DTAG(D), "RPC does not fit (%u bytes of payload)", (unsigned)plen);
    }
}

static void ask_controller_state(disp_t *D)
{
    uint8_t msg[8];
    pbw_t w = PBW(msg, sizeof(msg));
    uint8_t req[4];
    pbw_t r = PBW(req, sizeof(req));
    pbw_uint(&r, 2, 0); /* display_supports_send_log_report = false */
    pbw_bytes(&w, D2C_GET_CONTROLLER_STATE, r.buf, r.len);
    D->state_ticket = ++D->ticket;
    send_rpc(D, 1, D->state_ticket, 0, NULL, w.buf, w.len);
    if (D->state_reqs++ < 3) SLOGI(DTAG(D), "GetControllerStateRequest #%u sent", (unsigned)D->state_ticket);
    set_state(D, DS_ASKED);
}

/* DisplayToController #8 StartGame {1: key (max 23), 2: 1}. The controller holds the reply
 * until its game session connects (OK) or fails. */
static void send_start_game(disp_t *D)
{
    uint8_t rnd[8];
    port_random(rnd, sizeof(rnd));
    snprintf(D->game_key, sizeof(D->game_key), "local-%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3],
             rnd[4], rnd[5], rnd[6], rnd[7]);
    uint8_t sg[40], msg[48];
    pbw_t s = PBW(sg, sizeof(sg));
    pbw_str(&s, 1, D->game_key);
    pbw_uint(&s, 2, 1);
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, D2C_START_GAME, s.buf, s.len);
    D->start_ticket = ++D->ticket;
    D->start_at = port_now_ms();
    D->start_sent = true;
    send_rpc(D, 1, D->start_ticket, 0, NULL, m.buf, m.len);
    SLOGI(DTAG(D), "StartGame %s sent", D->game_key);
}

/* DisplayToController #16 SetAudioModeRequest {1 mode: 2, 3 f3: true}: the WebRTC Opus route to
 * the headset jack. Without it the controller takes the RTP and plays nothing.
 * (Its f2 is not the microphone: a controller on firmware 316382 answers INVALID_ARGUMENT.) */
static void send_set_audio_mode(disp_t *D)
{
    uint8_t am[8], msg[16];
    pbw_t a = PBW(am, sizeof(am));
    pbw_uint(&a, 1, AUDIO_MODE_WEBRTC_OPUS);
    pbw_uint(&a, 3, 1);
    pbw_t m = PBW(msg, sizeof(msg));
    pbw_bytes(&m, D2C_SET_AUDIO_MODE, a.buf, a.len);
    D->audio_ticket = ++D->ticket;
    D->audio_at = port_now_ms();
    D->audio_sent = true;
    D->audio_answered = false;
    send_rpc(D, 1, D->audio_ticket, 0, NULL, m.buf, m.len);
    SLOGI(DTAG(D), "SetAudioMode(WebRTC Opus) sent");
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
static void on_input(disp_t *D, const uint8_t *m, size_t len)
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
    set_state(D, DS_INPUT);
    pad_report(D->link.slot, &pad, DTAG(D));
}

/* ControllerToDisplay #1 ProxyRequest {1 f1, 2 key, 4 body: an inner RpcMessage request whose
 * payload is a StreamerRequest}. Answered with DisplayToController #1 ProxyResponse {1 data:
 * the inner RpcMessage response carrying a StreamerResponse}. */
static void on_proxy_request(disp_t *D, uint32_t ticket, const uint8_t *pr, size_t n)
{
    pbf_t body, init;
    rpc_msg_t inner;
    if (!pb_find(pr, n, 4, &body) || !rpc_decode(body.data, body.len, &inner) || inner.kind != 1 ||
        !inner.has_ticket || !inner.payload) {
        SLOGW(DTAG(D), "ProxyRequest with an unparsable body");
        send_rpc(D, 2, ticket, RPC_INVALID_ARGUMENT, "Unparsable proxied StreamerRequest", NULL, 0);
        return;
    }
    uint8_t sresp[800], inner_resp[900];
    size_t ilen;
    if (!pb_find(inner.payload, inner.plen, 1, &init) || init.wt != PB_LEN) {
        SLOGI(DTAG(D), "proxied StreamerRequest of an unknown kind: UNIMPLEMENTED");
        ilen = rpc_encode(2, inner.ticket, RPC_UNIMPLEMENTED, "Unimplemented StreamerRequest in local receiver", NULL, 0,
                          inner_resp, sizeof(inner_resp));
    } else {
        size_t slen = game_initialization(D->link.slot, init.data, init.len, sresp, sizeof(sresp));
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
        SLOGE(DTAG(D), "ProxyResponse does not fit");
        return;
    }
    send_rpc(D, 2, ticket, RPC_OK, "", m.buf, m.len);
}

static void on_rpc(disp_t *D, const uint8_t *data, size_t len)
{
    rpc_msg_t rpc;
    if (!rpc_decode(data, len, &rpc)) {
        SLOGW(DTAG(D), "undecodable RpcMessage (%u bytes)", (unsigned)len);
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
            set_state(D, DS_READY);
            pbf_t t;
            int64_t ctl_us = pb_find(variant.data, variant.len, 1, &t) ? (int64_t)t.v : 0;
            uint8_t ct[32], msg[40];
            pbw_t w = PBW(ct, sizeof(ct));
            pbw_sint64(&w, 1, ctl_us); /* echoed unchanged: the firmware CHECK-fails on rtt < 0 */
            pbw_sint64(&w, 2, (int64_t)(port_wall_time() * 1e6));
            pbw_t m = PBW(msg, sizeof(msg));
            pbw_bytes(&m, D2C_CURRENT_TIME, w.buf, w.len);
            send_rpc(D, 2, rpc.ticket, RPC_OK, "", m.buf, m.len);
            break;
        }
        case C2D_KEEPALIVE:
            set_state(D, DS_READY);
            send_rpc(D, 2, rpc.ticket, RPC_OK, "", NULL, 0);
            break;
        case C2D_DISCONNECT:
            SLOGI(DTAG(D), "the controller asks to disconnect");
            send_rpc(D, 2, rpc.ticket, RPC_OK, "", NULL, 0);
            break;
        case C2D_PROXY_REQUEST:
            on_proxy_request(D, rpc.ticket, variant.data, variant.len);
            break;
        default:
            SLOGI(DTAG(D), "request #%u (ControllerToDisplay #%u): answered UNIMPLEMENTED", (unsigned)rpc.ticket,
                  (unsigned)variant.field);
            send_rpc(D, 2, rpc.ticket, RPC_UNIMPLEMENTED, "Unimplemented RPC in local receiver", NULL, 0);
        }
    } else if (rpc.kind == 3) { /* notifications */
        if (variant.field == C2D_INPUT) {
            on_input(D, variant.data, variant.len);
        } else if (variant.field == C2D_BATTERY) {
            pbf_t v;
            if (pb_find(variant.data, variant.len, 1, &v)) {
                uint32_t bits = (uint32_t)v.v;
                float level;
                memcpy(&level, &bits, 4);
                if (!(level >= 0.0f)) level = 0.0f; /* NaN too */
                if (level > 1.0f) level = 1.0f;
                int pct = (int)(level * 100 + 0.5f);
                if (pct != D->battery_pct) SLOGI(DTAG(D), "battery %d%%", pct);
                D->battery_at = port_now_ms();
                D->battery_pct = pct;
            }
        } else if (variant.field == C2D_AUDIO_CONFIG) {
            pbf_t mode;
            bool in = pb_find(variant.data, variant.len, 1, &mode) && mode.v == 1;
            SLOGI(DTAG(D), "headset %s", in ? "plugged in" : "out");
        } else {
            SLOGI(DTAG(D), "notification ControllerToDisplay #%u", (unsigned)variant.field);
        }
    } else if (rpc.kind == 2 && D->link.slot == 0 && MIC.ticket && rpc.ticket == MIC.ticket) {
        if (rpc.code != 0 || MIC.sent <= 3 || g_cfg.mic_push_to_talk) {
            SLOGI(DTAG(D), "microphone %s answered: code %u", MIC.on ? "on" : "off", (unsigned)rpc.code);
        }
    } else if (rpc.kind == 2 && D->audio_sent && rpc.ticket == D->audio_ticket) {
        /* a repeat in the same boot is FAILED_PRECONDITION ("already in WebRTC Opus mode"): open too */
        SLOGI(DTAG(D), "SetAudioMode answered: code %u%s", (unsigned)rpc.code, rpc.code == 9 ? " (already open)" : "");
        D->audio_answered = true;
    } else if (rpc.kind == 2 && D->start_sent && rpc.ticket == D->start_ticket) {
        char msg[96] = "";
        pbf_t body;
        pbr_t r = PBR(data, len);
        if (pbr_next(&r, &body)) {
            pbr_t b = PBR(body.data, body.len);
            while (pbr_next(&b, &f)) {
                if (f.field == 3) pbf_str(&f, msg, sizeof(msg));
            }
        }
        int64_t after = port_now_ms() - D->start_at;
        if (rpc.code == 0) {
            SLOGI(DTAG(D), "StartGame answered OK after %d ms: the game session is up", (int)after);
        } else {
            SLOGW(DTAG(D), "StartGame failed after %d ms: code %u %s", (int)after, (unsigned)rpc.code, msg);
        }
    } else if (rpc.kind == 2 && rpc.ticket == D->state_ticket && D->state < DS_STATE) {
        if (rpc.code != 0 || variant.field != C2D_CONTROLLER_STATE) {
            if (D->state_reqs <= 3) {
                SLOGI(DTAG(D), "GetControllerState answered with code %u, asking again", (unsigned)rpc.code);
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
        SLOGI(DTAG(D), "controller: %s, hardware %s, firmware %s, headset %s", product, hw, fw, headset ? "in" : "out");
        set_state(D, DS_STATE);
    }
}

/* ---- display link callbacks --------------------------------------------------------- */

/* Input as RTP on sub-channel 0x66 (the fake controller's display-link input): acked with
 * InputProtocolMessage{ack_sequence_number} the same way. */
static void on_wire_rtp(disp_t *D, const uint8_t *p, size_t len)
{
    uint8_t pt;
    const uint8_t *pl;
    size_t pn;
    pbf_t series, seq;
    if (!rtp_payload(p, len, &pt, &pl, &pn) || !pb_find(pl, pn, 7, &series) ||
        !pb_find(series.data, series.len, 2, &seq)) {
        return;
    }
    set_state(D, DS_INPUT);
    SLOGI(DTAG(D), "input series #%llu on 0x66 (%u bytes)", (unsigned long long)seq.v, (unsigned)pn);
    uint8_t ack[16];
    pbw_t a = PBW(ack, sizeof(ack));
    pbw_uint(&a, 4, seq.v);
    rtc_link_send_rtp_wire(&D->link, pt, a.buf, a.len);
}

static void display_on_wire(rtc_link_t *L, uint16_t id, const uint8_t *p, size_t n)
{
    disp_t *D = DS[L->slot];
    if (id == WIRE_ID_RTP) {
        on_wire_rtp(D, p, n);
    } else if (id == WIRE_ID_RPC_CONTROLLER || id == WIRE_ID_RPC_DISPLAY) {
        on_rpc(D, p, n);
    } else {
        SLOGI(DTAG(D), "WireHeader2 id 0x%04x, %u bytes", id, (unsigned)n);
    }
}

static void display_on_connected(rtc_link_t *L)
{
    disp_t *D = DS[L->slot];
    D->state = DS_DTLS;
    D->next_state_req = L->dtls_at + DISPLAY_RPC_DELAY_MS;
    D->state_reqs = 0;
    D->ready_at = 0;
    D->start_sent = false;
    D->audio_sent = D->audio_answered = false; /* the microphone waits for this link's answer */
    if (L->slot == 0) {
        memset(&MIC, 0, sizeof(MIC));
        audio_set_route(false);
    }
}

static void display_on_closed(rtc_link_t *L)
{
    disp_t *D = DS[L->slot];
    D->state = DS_NONE;
    if (L->slot == 0) audio_set_route(false);
    stadia_pad_t released = {.connected = false};
    pad_report(L->slot, &released, DTAG(D));
}

static void display_on_media(rtc_link_t *L, const uint8_t *rtp, size_t n)
{
    if (L->slot == 0) mic_on_rtp(L, 0, rtp, n); /* the microphone is player 1's */
}

static const rtc_link_ops_t DISPLAY_OPS = {
    .on_connected = display_on_connected,
    .on_closed = display_on_closed,
    .on_wire = display_on_wire,
    .on_media = display_on_media,
};

/* ---- the task ----------------------------------------------------------------------- */

static void display_tick(disp_t *D)
{
    rtc_link_tick(&D->link);
    int64_t now = port_now_ms();
    bool up = rtc_link_up(&D->link);
    /* GetControllerStateRequest until answered */
    if (up && D->state < DS_STATE && now >= D->next_state_req && now - D->link.dtls_at < DISPLAY_RPC_WINDOW_MS) {
        ask_controller_state(D);
        D->next_state_req = now + DISPLAY_RPC_INTERVAL_MS;
    }
    /* StartGame once per connection, a second after READY */
    if (g_cfg.start_game && up && !D->start_sent && D->ready_at && now - D->ready_at >= START_GAME_DELAY_MS) {
        send_start_game(D);
    }
    if (D->link.slot != 0) return;
    /* player 1: the headset audio route, asked for at READY (before StartGame, as stadiacast does) */
    if (g_cfg.audio && up && D->ready_at && !D->audio_sent) send_set_audio_mode(D);
    if (D->audio_sent && up && (D->audio_answered || now - D->audio_at >= AUDIO_ROUTE_WAIT_MS)) {
        audio_set_route(true);
    }
    mic_tick(D, now);
    audio_tick(&D->link);
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
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        for (int i = 0; i < g_nctl; i++) {
            int a = DS[i]->link.sock, b = game_link(i)->sock;
            FD_SET(a, &fds);
            FD_SET(b, &fds);
            if (a > maxfd) maxfd = a;
            if (b > maxfd) maxfd = b;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = audio_poll_ms() * 1000};
        int r = select(maxfd + 1, &fds, NULL, NULL, &tv);
        for (int i = 0; i < g_nctl; i++) {
            rtc_link_t *G = game_link(i);
            if (r > 0 && FD_ISSET(DS[i]->link.sock, &fds)) poll_socket(&DS[i]->link, buf, cap);
            if (r > 0 && FD_ISSET(G->sock, &fds)) poll_socket(G, buf, cap);
        }
        for (int i = 0; i < g_nctl; i++) {
            display_tick(DS[i]);
            game_tick(i);
        }
    }
}

uint16_t rtc_display_port(int slot)
{
    return (uint16_t)(g_cfg.rtc_port + 2 * slot);
}

void rtc_set_remote(int slot, const remote_offer_t *remote)
{
    if (slot < 0 || slot >= g_nctl) return;
    game_reset(slot);
    rtc_link_set_remote(&DS[slot]->link, remote);
}

void rtc_forget(int slot)
{
    if (slot < 0 || slot >= g_nctl) return;
    game_reset(slot);
    rtc_link_close(game_link(slot));
    rtc_link_close(&DS[slot]->link);
}

bool rtc_start(void)
{
    if (!rtc_link_global_init()) return false;
    audio_init(&g_cfg);
    if (g_cfg.mic) mic_init();
    const rtc_identity_t *id = rtc_identity();
    for (int i = 0; i < g_nctl; i++) {
        /* player 1's link state where the audio path reads it fast; the others' in PSRAM */
        DS[i] = i == 0 ? port_alloc_fast(sizeof(disp_t)) : port_alloc_bulk(sizeof(disp_t));
        if (!DS[i]) {
            SLOGE(TAG, "no memory for player %d's links", i + 1);
            return false;
        }
        memset(DS[i], 0, sizeof(disp_t));
        DS[i]->battery_pct = -1;
        if (i == 0) {
            snprintf(DS[i]->name, sizeof(DS[i]->name), "display");
        } else {
            snprintf(DS[i]->name, sizeof(DS[i]->name), "display%d", i + 1);
        }
        if (!rtc_link_init(&DS[i]->link, DS[i]->name, i, rtc_display_port(i), &DISPLAY_OPS)) return false;
        rtc_link_set_credentials(&DS[i]->link, id->ufrag, id->pwd);
        if (!game_init(i, (uint16_t)(g_cfg.game_port + 2 * i))) return false;
    }
    return port_spawn(rtc_task, NULL, "rtc", 20 * 1024);
}

static const char *state_name(const disp_t *D)
{
    display_state_t ds = D->state;
    return DS_NAMES[ds <= DS_INPUT ? ds : DS_NONE];
}

void rtc_player_info(int slot, stadia_player_t *p)
{
    if (slot < 0 || slot >= g_nctl || !DS[slot]) return;
    const disp_t *D = DS[slot];
    p->display_state = state_name(D);
    p->display_up = rtc_link_up(&D->link);
    p->game_up = rtc_link_up(game_link(slot));
    p->battery_pct = D->battery_pct;
    p->ping_us = D->link.dtls_up ? D->link.ping_us : -1;
    if (D->link.dtls_up) {
        int64_t heard = D->link.last_rx > D->link.dtls_at ? D->link.last_rx : D->link.dtls_at;
        int32_t s = (int32_t)((port_now_ms() - heard) / 1000);
        if (p->seen_s < 0 || s < p->seen_s) p->seen_s = s;
    }
    stadia_stats_t st = {0};
    game_get_stats(slot, &st);
    p->input_series = st.input_series;
    p->input_gaps = st.input_gaps;
}

void stadia_get_stats(stadia_stats_t *st)
{
    *st = (stadia_stats_t){.mic_age_ms = -1, .battery_pct = -1, .battery_age_s = -1, .ping_us = {-1, -1},
                           .display_state = DS_NAMES[DS_NONE]};
    audio_get_stats(st);
    if (g_cfg.mic) mic_get_stats(st);
    const disp_t *D = DS[0];
    if (!D) return;
    st->display_state = state_name(D);
    st->display_up = rtc_link_up(&D->link);
    st->game_up = rtc_link_up(game_link(0));
    int64_t now = port_now_ms();
    int pct = D->battery_pct;
    int64_t at = D->battery_at;
    st->battery_pct = pct;
    st->battery_age_s = pct < 0 ? -1 : (int32_t)((now - at) / 1000);
    const rtc_link_t *links[2] = {&D->link, game_link(0)};
    for (int i = 0; i < 2; i++) {
        st->ping_us[i] = links[i]->dtls_up ? links[i]->ping_us : -1;
        st->pings_sent += links[i]->pings_sent;
        st->pings_answered += links[i]->pings_answered;
    }
    game_get_stats(0, st);
}
