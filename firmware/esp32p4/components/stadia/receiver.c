/*
 * The Google endpoints the controller calls at boot, and its discovery session
 * (port of receiver.py's routing and controller.py's discovery RPCs and GMsg channel).
 *
 * Several controllers: each one gets a player number (a slot) the first time it calls an RPC,
 * found again by the ids the firmware sends (the body's device_id, the controller-device-id and
 * controller-cloud-device-id headers), the display token we handed it (its GMsg subscription)
 * or, failing those, its address. Each slot has its own GMsg queue and its own pair of UDP
 * links (rtc.c). A controller seen before (stadia_config_t.known_ids) gets its old number.
 */
#include "stadia_internal.h"
#include "stadia_pb.h"
#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "receiver";

#define DISPLAY_ID STADIA_SERVER_NAME
#define REGISTRATION_TOKEN STADIA_SERVER_NAME
#define DISPLAY_TOKEN_PREFIX STADIA_SERVER_NAME "-display-"
#define GMSG_COMMAND_TYPE 42
#define GMSG_CONTROLLER_COMMAND 269197544u
#define GMSG_HOLD_MS 20000
#define CLEARCUT_WAIT_MS (30 * 60 * 1000)
#define IDS 3 /* body device_id, controller-device-id, controller-cloud-device-id */

static const char PB[] = "application/x-protobuf";

typedef struct {
    bool used;
    char ids[IDS][STADIA_DEVICE_ID_MAX]; /* every id it has sent; ids[0] the one reported */
    char ip[16];
    char token[48]; /* its display token, the GMsg subscription it polls with */
    int64_t last_seen;
    remote_offer_t remote;
    bool resolved;
    /* GMsg: one pending message, re-sent until acked */
    int64_t gmsg_seq;
    int64_t pending_id;
    uint8_t pending[1536];
    size_t pending_len;
    port_event_t *gmsg_event;
} sess_t;

static struct {
    int n;          /* slots this start has (g_cfg.max_controllers) */
    sess_t *s;      /* n of them */
    char known[STADIA_MAX_CONTROLLERS][STADIA_DEVICE_ID_MAX]; /* the id each player had before */
    stadia_controller_cb_t cb;
    void *cb_arg;
} R;

int g_nctl = 1;

/* ---- which controller ----------------------------------------------------------------- */

static bool has_id(const sess_t *c, const char *id)
{
    if (!id || !id[0]) return false;
    for (int k = 0; k < IDS; k++) {
        if (c->ids[k][0] && strcmp(c->ids[k], id) == 0) return true;
    }
    return false;
}

static bool any_id(const sess_t *c)
{
    return c->ids[0][0] != 0;
}

static void add_id(sess_t *c, const char *id)
{
    if (!id || !id[0] || has_id(c, id)) return;
    for (int k = 0; k < IDS; k++) {
        if (!c->ids[k][0]) {
            snprintf(c->ids[k], sizeof(c->ids[k]), "%s", id);
            return;
        }
    }
}

/* The slot a request belongs to, under the lock; -1 if none. */
static int known_slot(const char *token, const char *const ids[IDS], const char *ip)
{
    bool have_ids = false;
    for (int k = 0; k < IDS; k++) have_ids |= ids[k] && ids[k][0];
    if (token && token[0]) {
        for (int i = 0; i < R.n; i++) {
            if (R.s[i].used && strcmp(R.s[i].token, token) == 0) return i;
        }
    }
    for (int k = 0; k < IDS; k++) {
        for (int i = 0; i < R.n; i++) {
            if (R.s[i].used && has_id(&R.s[i], ids[k])) return i;
        }
    }
    if (ip && ip[0]) {
        for (int i = 0; i < R.n; i++) {
            /* the same address and no ids to tell it apart: the controller we know there */
            if (R.s[i].used && strcmp(R.s[i].ip, ip) == 0 && (!have_ids || !any_id(&R.s[i]))) return i;
        }
    }
    return -1;
}

static void forget_slot(int i)
{
    sess_t *c = &R.s[i];
    port_event_t *ev = c->gmsg_event;
    int64_t seq = c->gmsg_seq;
    memset(c, 0, sizeof(*c));
    c->gmsg_event = ev;
    c->gmsg_seq = seq; /* GMsg ids stay unique across owners */
    snprintf(c->token, sizeof(c->token), DISPLAY_TOKEN_PREFIX "%d", i);
}

/* A new controller: the slot it had before, else a free one nobody had, else a free one,
 * else the one heard from the longest ago (its links close). Under the lock. */
static int new_slot(const char *const ids[IDS], const char *ip, int *evicted)
{
    *evicted = -1;
    for (int k = 0; k < IDS; k++) {
        if (!ids[k] || !ids[k][0]) continue;
        for (int i = 0; i < R.n; i++) {
            if (!R.s[i].used && strcmp(R.known[i], ids[k]) == 0) return i;
        }
    }
    for (int i = 0; i < R.n; i++) {
        if (!R.s[i].used && !R.known[i][0]) return i;
    }
    for (int i = 0; i < R.n; i++) {
        if (!R.s[i].used) return i;
    }
    int oldest = 0;
    for (int i = 1; i < R.n; i++) {
        if (R.s[i].last_seen < R.s[oldest].last_seen) oldest = i;
    }
    *evicted = oldest;
    return oldest;
}

/* The slot of the controller behind a request, created on first sight when `create` is set
 * (RPCs, not GMsg polls). -1: none. */
static int controller_for(const http_req_t *req, const char *body_id, const char *token, bool create)
{
    const char *ids[IDS] = {body_id, req->cloud_device_id, req->device_id};
    char report[STADIA_DEVICE_ID_MAX] = "";
    int evicted = -1;
    port_lock();
    int i = known_slot(token, ids, req->peer);
    bool fresh = false;
    if (i < 0 && create) {
        i = new_slot(ids, req->peer, &evicted);
        forget_slot(i);
        R.s[i].used = true;
        fresh = true;
    }
    if (i >= 0) {
        sess_t *c = &R.s[i];
        for (int k = 0; k < IDS; k++) add_id(c, ids[k]);
        if (req->peer[0]) snprintf(c->ip, sizeof(c->ip), "%s", req->peer);
        c->last_seen = port_now_ms();
        /* a controller with an id the slot did not have before: report it (once it has one) */
        if (any_id(c) && strcmp(R.known[i], c->ids[0]) != 0 && !has_id(c, R.known[i])) {
            snprintf(R.known[i], sizeof(R.known[i]), "%s", c->ids[0]);
            snprintf(report, sizeof(report), "%s", c->ids[0]);
        }
    }
    stadia_controller_cb_t cb = R.cb;
    void *arg = R.cb_arg;
    port_unlock();
    if (fresh) {
        if (evicted >= 0) {
            SLOGW(TAG, "every player is taken: player %d goes to a new controller", evicted + 1);
            rtc_forget(evicted);
        }
        SLOGI(TAG, "player %d: a controller at %s (%s)", i + 1, req->peer, ids[0] && ids[0][0] ? ids[0] : "no id yet");
    }
    if (report[0]) {
        SLOGI(TAG, "player %d is controller %s", i + 1, report);
        if (cb) cb(i, report, arg);
    }
    return i;
}

void stadia_set_controller_callback(stadia_controller_cb_t cb, void *arg)
{
    port_lock();
    R.cb = cb;
    R.cb_arg = arg;
    port_unlock();
}

void stadia_forget_player(int player)
{
    if (player < 0 || player >= R.n || !R.s) return;
    port_lock();
    forget_slot(player);
    R.known[player][0] = '\0';
    stadia_controller_cb_t cb = R.cb;
    void *arg = R.cb_arg;
    port_unlock();
    rtc_forget(player);
    SLOGI(TAG, "player %d forgotten", player + 1);
    if (cb) cb(player, "", arg);
}

int stadia_get_players(stadia_player_t *out, int max)
{
    int64_t now = port_now_ms();
    for (int i = 0; i < max && i < R.n; i++) {
        stadia_player_t *p = &out[i];
        memset(p, 0, sizeof(*p));
        port_lock();
        const sess_t *c = &R.s[i];
        p->known = c->used || R.known[i][0];
        snprintf(p->device_id, sizeof(p->device_id), "%s", c->used && any_id(c) ? c->ids[0] : R.known[i]);
        snprintf(p->ip, sizeof(p->ip), "%s", c->used ? c->ip : "");
        p->seen_s = c->used && c->last_seen ? (int32_t)((now - c->last_seen) / 1000) : -1;
        port_unlock();
        rtc_player_info(i, p);
        if (p->display_up && p->seen_s < 0) p->seen_s = 0;
    }
    return R.n;
}

/* ---- boot-time endpoints ------------------------------------------------------------ */

static void omaha(const http_req_t *req, http_resp_t *resp, uint8_t *out, size_t cap)
{
    double now = port_wall_time();
    long secs = (long)now;
    long elapsed_days = (secs - 1167609600L) / 86400; /* days since 2007-01-01 */
    size_t n = (size_t)snprintf((char *)out, cap,
                                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><response protocol=\"3.0\" server=\"%s\">"
                                "<daystart elapsed_seconds=\"%ld\" elapsed_days=\"%ld\"/>",
                                STADIA_SERVER_NAME, secs % 86400, elapsed_days);
    /* every <app ... appid="X"> in the request gets "noupdate" */
    char body[4096];
    size_t blen = req->body_len < sizeof(body) - 1 ? req->body_len : sizeof(body) - 1;
    memcpy(body, req->body, blen);
    body[blen] = '\0';
    int apps = 0;
    for (char *p = strstr(body, "<app "); p && apps < 8; p = strstr(p + 5, "<app ")) {
        char *gt = strchr(p, '>');
        char *id = strstr(p, "appid=\"");
        if (!id || (gt && id > gt)) {
            continue;
        }
        id += 7;
        size_t idlen = strcspn(id, "\"<>&");
        n += (size_t)snprintf((char *)out + n, n < cap ? cap - n : 0,
                              "<app appid=\"%.*s\" status=\"ok\"><updatecheck status=\"noupdate\"/><ping status=\"ok\"/></app>",
                              (int)idlen, id);
        apps++;
    }
    if (!apps) {
        n += (size_t)snprintf((char *)out + n, n < cap ? cap - n : 0,
                              "<app appid=\"{00000000-0000-0000-0000-000000000000}\" status=\"ok\">"
                              "<updatecheck status=\"noupdate\"/><ping status=\"ok\"/></app>");
    }
    n += (size_t)snprintf((char *)out + n, n < cap ? cap - n : 0, "</response>");
    *resp = (http_resp_t){200, "text/xml; charset=utf-8", out, n < cap ? n : cap, false, "omaha"};
}

/* ---- discovery RPCs ----------------------------------------------------------------- */

/* Accept a controller's offer bundle and queue our answer for its next GMsg poll. */
static void accept_offer(int slot, const uint8_t *bundle, size_t len)
{
    sess_t *c = &R.s[slot];
    uint8_t *answer = malloc(sizeof(c->pending));
    if (!answer) return;
    remote_offer_t remote;
    size_t n = signaling_answer(bundle, len, &remote, rtc_display_port(slot), answer, sizeof(c->pending));
    if (!n) {
        free(answer);
        return;
    }
    rtc_set_remote(slot, &remote);
    port_lock();
    c->remote = remote;
    c->resolved = false;
    c->gmsg_seq++;
    c->pending_id = c->gmsg_seq;
    memcpy(c->pending, answer, n);
    c->pending_len = n;
    int64_t id = c->pending_id;
    port_unlock();
    free(answer);
    port_event_set(c->gmsg_event);
    SLOGI(TAG, "player %d: answer queued as GMsg #%lld", slot + 1, (long long)id);
}

/* DiscoverDisplayResponse-like display block: {1 display_id, 4 display_token} */
static size_t display_block(int slot, uint8_t *out, size_t cap)
{
    pbw_t d = PBW(out, cap);
    pbw_str(&d, 1, DISPLAY_ID);
    pbw_str(&d, 4, R.s[slot].token);
    return d.len;
}

static void register_controller(const http_req_t *req, http_resp_t *resp, uint8_t *out, size_t cap)
{
    char dev[STADIA_DEVICE_ID_MAX] = "", attempt[64] = "";
    pbf_t f, discovery = {0};
    pbr_t r = PBR(req->body, req->body_len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) pbf_str(&f, dev, sizeof(dev));
        if (f.field == 4) pbf_str(&f, attempt, sizeof(attempt));
        if (f.field == 5 && f.wt == PB_LEN) discovery = f;
    }
    int slot = controller_for(req, dev, NULL, true);

    char reg_id[64];
    snprintf(reg_id, sizeof(reg_id), "local-%.40s", dev[0] ? dev : "controller");
    pbw_t w = PBW(out, cap);
    pbw_str(&w, 1, reg_id);
    pbw_sint64(&w, 2, 1000); /* poll_interval_ms: required, > 0 */
    pbw_str(&w, 3, REGISTRATION_TOKEN);
    if (discovery.data && slot >= 0) {
        /* Autolinking boot: the display oneof makes the controller go on to GMsg and ICE.
         * A registration without a discovery must be answered without it (protocol.md 11). */
        uint8_t disp[96];
        size_t dn = display_block(slot, disp, sizeof(disp));
        pbw_bytes(&w, 5, disp, dn);
        pbf_t offer;
        if (pb_find(discovery.data, discovery.len, 4, &offer) && offer.len) {
            accept_offer(slot, offer.data, offer.len);
        }
    }
    SLOGI(TAG, "RegisterController from %s (player %d, attempt %s)%s", dev, slot + 1, attempt,
          discovery.data ? " with a resumed discovery and offer" : ", no discovery");
    *resp = (http_resp_t){200, PB, out, w.len, false, "rpc:RegisterController"};
}

static void discover_display(const http_req_t *req, http_resp_t *resp, uint8_t *out, size_t cap)
{
    char dev[STADIA_DEVICE_ID_MAX] = "";
    pbf_t f;
    if (pb_find(req->body, req->body_len, 1, &f)) pbf_str(&f, dev, sizeof(dev));
    int slot = controller_for(req, dev, NULL, true);
    if (slot < 0) {
        *resp = (http_resp_t){400, NULL, NULL, 0, false, "rpc:DiscoverDisplay"};
        return;
    }
    pbf_t offer;
    if (pb_find(req->body, req->body_len, 4, &offer) && offer.len) {
        accept_offer(slot, offer.data, offer.len);
    }
    size_t n = display_block(slot, out, cap);
    *resp = (http_resp_t){200, PB, out, n, false, "rpc:DiscoverDisplay"};
}

static void cloudcast_rpc(const http_req_t *req, const char *method, http_resp_t *resp, uint8_t *out, size_t cap)
{
    if (strcmp(req->method, "POST") != 0) {
        *resp = (http_resp_t){405, NULL, NULL, 0, false, "rpc"};
        return;
    }
    if (strcmp(method, "FirmwareVersionCheck") == 0) {
        pbw_t w = PBW(out, cap);
        pbw_uint(&w, 1, 0); /* requires_update = false */
        *resp = (http_resp_t){200, PB, out, w.len, false, "rpc:FirmwareVersionCheck"};
    } else if (strcmp(method, "RegisterController") == 0) {
        register_controller(req, resp, out, cap);
    } else if (strcmp(method, "DiscoverDisplay") == 0) {
        discover_display(req, resp, out, cap);
    } else if (strcmp(method, "ResolveDiscovery") == 0) {
        /* The firmware ignores the body: any 200 ends discovery. */
        char dev[STADIA_DEVICE_ID_MAX] = "";
        pbf_t f;
        if (pb_find(req->body, req->body_len, 1, &f)) pbf_str(&f, dev, sizeof(dev));
        int slot = controller_for(req, dev, NULL, false);
        if (slot >= 0) {
            port_lock();
            R.s[slot].resolved = true;
            port_unlock();
        }
        SLOGI(TAG, "ResolveDiscovery (player %d): discovery complete", slot + 1);
        *resp = (http_resp_t){200, PB, NULL, 0, false, "rpc:ResolveDiscovery"};
    } else {
        SLOGW(TAG, "unknown RPC %s: empty answer", method);
        *resp = (http_resp_t){200, PB, NULL, 0, false, "rpc:unknown"};
    }
}

/* ---- GMsg push channel -------------------------------------------------------------- */

static void gmsg(const http_req_t *req, http_resp_t *resp, uint8_t *out, size_t cap)
{
    char sub[72] = "";
    bool has_ack = false;
    int64_t ack = 0;
    pbf_t f;
    pbr_t r = PBR(req->body, req->body_len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) pbf_str(&f, sub, sizeof(sub));
        if (f.field == 2 && f.wt == PB_LEN) {
            pbf_t id;
            if (pb_find(f.data, f.len, 1, &id)) {
                has_ack = true;
                ack = (int64_t)id.v;
            }
        }
    }
    int slot = controller_for(req, NULL, sub, false);
    if (slot < 0) {
        SLOGW(TAG, "GMsg poll from %s (%s) that never registered: empty answer", req->peer, sub[0] ? sub : "no subscription");
        *resp = (http_resp_t){200, PB, NULL, 0, false, "gmsg:unattributed"};
        return;
    }
    sess_t *c = &R.s[slot];
    port_lock();
    if (has_ack && c->pending_len && ack == c->pending_id) {
        SLOGI(TAG, "player %d: GMsg #%lld acked", slot + 1, (long long)ack);
        c->pending_len = 0;
    }
    bool idle = c->pending_len == 0;
    port_unlock();

    /* Long poll: hold an idle poll until something is queued (the live preset's gmsg_hold). */
    int64_t until = port_now_ms() + GMSG_HOLD_MS;
    while (idle) {
        int64_t left = until - port_now_ms();
        if (left <= 0) break;
        port_event_wait(c->gmsg_event, (int)left);
        port_lock();
        idle = c->pending_len == 0;
        port_unlock();
    }
    if (idle) {
        *resp = (http_resp_t){200, PB, NULL, 0, false, "gmsg:idle"};
        return;
    }

    /* GmsgEnvelope{message{id, type=42, [controller_command]{payload}}} */
    uint8_t *cmd = malloc(sizeof(c->pending) + 16), *msg = malloc(sizeof(c->pending) + 48);
    if (!cmd || !msg) {
        free(cmd);
        free(msg);
        *resp = (http_resp_t){200, PB, NULL, 0, false, "gmsg:idle"};
        return;
    }
    port_lock();
    int64_t id = c->pending_id;
    pbw_t cw = PBW(cmd, sizeof(c->pending) + 16);
    pbw_bytes(&cw, 1, c->pending, c->pending_len);
    port_unlock();
    pbw_t m = PBW(msg, sizeof(c->pending) + 48);
    pbw_sint64(&m, 1, id);
    pbw_uint(&m, 2, GMSG_COMMAND_TYPE);
    pbw_bytes(&m, GMSG_CONTROLLER_COMMAND, cw.buf, cw.len);
    pbw_t e = PBW(out, cap);
    pbw_bytes(&e, 1, m.buf, m.len);
    free(cmd);
    free(msg);
    SLOGI(TAG, "player %d: GMsg #%lld delivered to %s", slot + 1, (long long)id, sub[0] ? sub : "(no subscription)");
    *resp = (http_resp_t){200, PB, out, e.overflow ? 0 : e.len, false, "gmsg:sdp_answer"};
}

/* ---- routing ------------------------------------------------------------------------ */

void receiver_handle(const http_req_t *req, http_resp_t *resp, uint8_t *scratch, size_t cap)
{
    const char *path = req->path;
    static const char RPC[] = "/v1:";
    if (strcmp(path, "/generate_204") == 0 || strncasecmp(req->host, "connectivitycheck.", 18) == 0) {
        *resp = (http_resp_t){204, NULL, NULL, 0, false, "netcheck"};
    } else if (strncmp(path, "/cast/chromecast/device/stadiaconfig", 36) == 0) {
        static const uint8_t EMPTY_JSON[] = "{}";
        *resp = (http_resp_t){200, "application/json", EMPTY_JSON, 2, false, "stadiaconfig"};
    } else if (strcmp(path, "/service/update2") == 0) {
        omaha(req, resp, scratch, cap);
    } else if (strcmp(path, "/log/batch") == 0) {
        /* Clearcut over a connection we did not refuse: LogResponse with a 30 min wait, then
         * close, since a kept-alive connection here once blocked ResolveDiscovery. */
        pbw_t w = PBW(scratch, cap);
        pbw_sint64(&w, 1, CLEARCUT_WAIT_MS);
        *resp = (http_resp_t){200, "application/octet-stream", scratch, w.len, true, "clearcut"};
    } else if (strcmp(path, "/gmsg") == 0 || strncasecmp(req->host, "cloudcast-gmsg", 14) == 0) {
        gmsg(req, resp, scratch, cap);
    } else if (strncmp(path, RPC, 4) == 0) {
        cloudcast_rpc(req, path + 4, resp, scratch, cap);
    } else if (strncmp(path, "/cr/report", 10) == 0 || strcmp(path, "/setup/send_log_report") == 0) {
        *resp = (http_resp_t){200, "text/plain", NULL, 0, false, "crash_report"};
    } else {
        *resp = (http_resp_t){404, NULL, NULL, 0, false, "not_found"};
    }
}

/* ---- start everything --------------------------------------------------------------- */

stadia_config_t g_cfg;

bool stadia_start(const stadia_config_t *cfg)
{
    /* keep our own copy of the address: the caller's string may live on a stack that goes away */
    static char advertise_ip[16];
    g_cfg = *cfg;
    snprintf(advertise_ip, sizeof(advertise_ip), "%s", cfg->advertise_ip ? cfg->advertise_ip : "");
    g_cfg.advertise_ip = advertise_ip;
    if (!g_cfg.game_port) g_cfg.game_port = g_cfg.rtc_port + 1;
    int n = cfg->max_controllers ? cfg->max_controllers : 1;
    if (n > STADIA_MAX_CONTROLLERS) n = STADIA_MAX_CONTROLLERS;
    g_cfg.max_controllers = (uint8_t)n;
    g_nctl = R.n = n;
    /* the sessions hold GMsg buffers the CPU only copies: PSRAM is fine */
    R.s = port_alloc_bulk(sizeof(sess_t) * (size_t)n);
    if (!R.s) return false;
    for (int i = 0; i < n; i++) {
        R.s[i].gmsg_event = port_event_new();
        if (!R.s[i].gmsg_event) return false;
        forget_slot(i);
    }
    for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) {
        snprintf(R.known[i], sizeof(R.known[i]), "%s", i < n && cfg->known_ids[i] ? cfg->known_ids[i] : "");
        g_cfg.known_ids[i] = NULL; /* the caller's strings: not kept */
    }
    if (!rtc_identity_init()) {
        return false;
    }
    bool ok = rtc_start();
    ok = https_start(cfg->https_port) && ok;
    if (cfg->dns_port) ok = dns_sink_start(cfg->dns_port, cfg->advertise_ip) && ok;
    if (cfg->sntp_port) ok = sntp_start(cfg->sntp_port) && ok;
    return ok;
}
