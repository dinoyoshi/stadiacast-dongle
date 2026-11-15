/*
 * The Google endpoints the controller calls at boot, and its discovery session
 * (port of receiver.py's routing and controller.py's discovery RPCs and GMsg channel).
 * The dongle serves one controller, so the session is a single global.
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
#define DISPLAY_TOKEN STADIA_SERVER_NAME "-display-0"
#define GMSG_COMMAND_TYPE 42
#define GMSG_CONTROLLER_COMMAND 269197544u
#define GMSG_HOLD_MS 20000
#define CLEARCUT_WAIT_MS (30 * 60 * 1000)

static const char PB[] = "application/x-protobuf";

static struct {
    char device_id[64];
    remote_offer_t remote;
    bool resolved;
    /* GMsg: one pending message, re-sent until acked */
    int64_t gmsg_seq;
    int64_t pending_id;
    uint8_t pending[1536];
    size_t pending_len;
    port_event_t *gmsg_event;
} S;

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

/* Accept the controller's offer bundle and queue our answer for its next GMsg poll. */
static void accept_offer(const uint8_t *bundle, size_t len)
{
    uint8_t answer[sizeof(S.pending)];
    remote_offer_t remote;
    size_t n = signaling_answer(bundle, len, &remote, answer, sizeof(answer));
    if (!n) {
        return;
    }
    rtc_set_remote(&remote);
    port_lock();
    S.remote = remote;
    S.resolved = false;
    S.gmsg_seq++;
    S.pending_id = S.gmsg_seq;
    memcpy(S.pending, answer, n);
    S.pending_len = n;
    port_unlock();
    port_event_set(S.gmsg_event);
    SLOGI(TAG, "answer queued as GMsg #%lld", (long long)S.pending_id);
}

static void register_controller(const http_req_t *req, http_resp_t *resp, uint8_t *out, size_t cap)
{
    char dev[64] = "", attempt[64] = "";
    pbf_t f, discovery = {0};
    pbr_t r = PBR(req->body, req->body_len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) pbf_str(&f, dev, sizeof(dev));
        if (f.field == 4) pbf_str(&f, attempt, sizeof(attempt));
        if (f.field == 5 && f.wt == PB_LEN) discovery = f;
    }
    port_lock();
    snprintf(S.device_id, sizeof(S.device_id), "%s", dev);
    port_unlock();

    char reg_id[64];
    snprintf(reg_id, sizeof(reg_id), "local-%.40s", dev[0] ? dev : "controller");
    pbw_t w = PBW(out, cap);
    pbw_str(&w, 1, reg_id);
    pbw_sint64(&w, 2, 1000); /* poll_interval_ms: required, > 0 */
    pbw_str(&w, 3, REGISTRATION_TOKEN);
    if (discovery.data) {
        /* Autolinking boot: the display oneof makes the controller go on to GMsg and ICE.
         * A registration without a discovery must be answered without it (protocol.md 11). */
        uint8_t disp[96];
        pbw_t d = PBW(disp, sizeof(disp));
        pbw_str(&d, 1, DISPLAY_ID);
        pbw_str(&d, 4, DISPLAY_TOKEN);
        pbw_bytes(&w, 5, d.buf, d.len);
        pbf_t offer;
        if (pb_find(discovery.data, discovery.len, 4, &offer) && offer.len) {
            accept_offer(offer.data, offer.len);
        }
    }
    SLOGI(TAG, "RegisterController from %s (attempt %s)%s", dev, attempt,
          discovery.data ? " with a resumed discovery and offer" : ", no discovery");
    *resp = (http_resp_t){200, PB, out, w.len, false, "rpc:RegisterController"};
}

static void discover_display(const http_req_t *req, http_resp_t *resp, uint8_t *out, size_t cap)
{
    pbf_t offer;
    if (pb_find(req->body, req->body_len, 4, &offer) && offer.len) {
        accept_offer(offer.data, offer.len);
    }
    pbw_t w = PBW(out, cap);
    pbw_str(&w, 1, DISPLAY_ID);
    pbw_str(&w, 4, DISPLAY_TOKEN);
    *resp = (http_resp_t){200, PB, out, w.len, false, "rpc:DiscoverDisplay"};
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
        port_lock();
        S.resolved = true;
        port_unlock();
        SLOGI(TAG, "ResolveDiscovery: discovery complete");
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
    port_lock();
    if (has_ack && S.pending_len && ack == S.pending_id) {
        SLOGI(TAG, "GMsg #%lld acked", (long long)ack);
        S.pending_len = 0;
    }
    bool idle = S.pending_len == 0;
    port_unlock();

    /* Long poll: hold an idle poll until something is queued (the live preset's gmsg_hold). */
    int64_t until = port_now_ms() + GMSG_HOLD_MS;
    while (idle) {
        int64_t left = until - port_now_ms();
        if (left <= 0) break;
        port_event_wait(S.gmsg_event, (int)left);
        port_lock();
        idle = S.pending_len == 0;
        port_unlock();
    }
    if (idle) {
        *resp = (http_resp_t){200, PB, NULL, 0, false, "gmsg:idle"};
        return;
    }

    /* GmsgEnvelope{message{id, type=42, [controller_command]{payload}}} */
    uint8_t *cmd = malloc(sizeof(S.pending) + 16), *msg = malloc(sizeof(S.pending) + 48);
    port_lock();
    int64_t id = S.pending_id;
    pbw_t c = PBW(cmd, sizeof(S.pending) + 16);
    pbw_bytes(&c, 1, S.pending, S.pending_len);
    port_unlock();
    pbw_t m = PBW(msg, sizeof(S.pending) + 48);
    pbw_sint64(&m, 1, id);
    pbw_uint(&m, 2, GMSG_COMMAND_TYPE);
    pbw_bytes(&m, GMSG_CONTROLLER_COMMAND, c.buf, c.len);
    pbw_t e = PBW(out, cap);
    pbw_bytes(&e, 1, m.buf, m.len);
    free(cmd);
    free(msg);
    SLOGI(TAG, "GMsg #%lld delivered to %s", (long long)id, sub[0] ? sub : "(no subscription)");
    *resp = (http_resp_t){200, PB, out, e.overflow ? 0 : e.len, false, "gmsg:sdp_answer"};
}

/* ---- routing ------------------------------------------------------------------------ */

void receiver_handle(const http_req_t *req, http_resp_t *resp, uint8_t *scratch, size_t cap)
{
    if (!S.gmsg_event) {
        port_lock();
        if (!S.gmsg_event) S.gmsg_event = port_event_new();
        port_unlock();
    }
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
    S.gmsg_event = port_event_new();
    if (!rtc_identity_init()) {
        return false;
    }
    bool ok = rtc_start(cfg->rtc_port);
    ok = https_start(cfg->https_port) && ok;
    if (cfg->dns_port) ok = dns_sink_start(cfg->dns_port, cfg->advertise_ip) && ok;
    if (cfg->sntp_port) ok = sntp_start(cfg->sntp_port) && ok;
    return ok;
}
