/*
 * The display link on UDP (port of rtc/endpoint.py, rtc/stun.py, rtc/wire.py and the
 * post-DTLS half of controller.py):
 *
 *   ICE      answer the controller's STUN binding requests, send our own checks (we are the
 *            controlled agent), every 1 s until DTLS starts and every 2.5 s after
 *   DTLS     server role (the live preset's dtls_role=server), use_srtp, SRTP keys exported
 *   RPCs     WireHeader2 records; GetControllerStateRequest every 1.5 s from 0.6 s after
 *            DTLS until answered, then the controller's READY traffic: GetCurrentTime and
 *            keepalive requests answered, input notifications decoded
 *
 * Everything on the socket runs in one task, so only the remote description (set from an
 * HTTPS task) needs the lock.
 */
#include "mbedtls/private_access.h"
#include "stadia_internal.h"
#include "stadia_pb.h"
#include "stadia_port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "rtc";

/* ---- STUN (RFC 5389 subset) -------------------------------------------------------- */

#define STUN_MAGIC 0x2112A442u
#define BINDING_REQUEST 0x0001
#define BINDING_SUCCESS 0x0101
#define BINDING_ERROR 0x0111
#define ATTR_USERNAME 0x0006
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_ERROR_CODE 0x0009
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_PRIORITY 0x0024
#define ATTR_USE_CANDIDATE 0x0025
#define ATTR_FINGERPRINT 0x8028
#define ATTR_ICE_CONTROLLED 0x8029
#define HOST_CANDIDATE_PRIORITY 2130706431u

/* ---- WireHeader2 and the display RPCs ----------------------------------------------- */

#define WIRE_HEADER_LEN 8
#define WIRE_ID_RTP 0x66
#define WIRE_ID_RPC_CONTROLLER 0x74
#define WIRE_ID_RPC_DISPLAY 0x75 /* ours: the live preset's rpc_wire_id */

enum { RPC_OK = 0, RPC_UNIMPLEMENTED = 12 };
/* ControllerToDisplay variants */
enum { C2D_PROXY_REQUEST = 1, C2D_CONTROLLER_STATE = 2, C2D_INPUT = 3, C2D_AUDIO_CONFIG = 4, C2D_DISCONNECT = 8,
       C2D_BATTERY = 10, C2D_KEEPALIVE = 14, C2D_GET_CURRENT_TIME = 18 };
/* DisplayToController fields */
enum { D2C_GET_CONTROLLER_STATE = 2, D2C_CURRENT_TIME = 21 };

#define DISPLAY_RPC_DELAY_MS 600
#define DISPLAY_RPC_INTERVAL_MS 1500
#define DISPLAY_RPC_WINDOW_MS (300 * 1000)
#define FAST_CHECK_MS 1000
#define FAST_CHECK_WINDOW_MS 30000
#define KEEPALIVE_MS 2500

typedef enum { DS_NONE, DS_DTLS, DS_ASKED, DS_STATE, DS_READY, DS_INPUT } display_state_t;
static const char *const DS_NAMES[] = {"none", "dtls-connected", "get-controller-state-sent",
                                       "controller-state-received", "ready", "input"};

static struct {
    int sock;
    /* remote description, set by rtc_set_remote() from an HTTPS task */
    bool remote_new;
    remote_offer_t remote;
    /* ICE */
    bool have_remote;
    struct sockaddr_in cand[4];
    int ncand;
    int check_attempts[4];
    int64_t next_check[4];
    bool selected;
    struct sockaddr_in peer;
    bool verified_out;
    int64_t fast_until, next_keepalive;
    uint8_t tiebreaker[8];
    /* DTLS */
    bool dtls_on, dtls_up;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt own_cert;
    mbedtls_pk_context own_key;
    const uint8_t *in_dgram;
    size_t in_len;
    int64_t timer_int, timer_fin, timer_start;
    uint8_t master[48], randbytes[64];
    mbedtls_tls_prf_types prf;
    uint8_t srtp_keys[60]; /* client key 16, server key 16, client salt 14, server salt 14 */
    bool srtp_ready;
    int64_t dtls_at;
    /* display RPCs */
    display_state_t state;
    uint32_t ticket;
    uint32_t state_ticket; /* our last GetControllerStateRequest */
    int64_t next_state_req;
    int state_reqs;
    /* input */
    uint32_t last_buttons;
    bool have_input;
    uint16_t tx_seq;
    uint32_t ssrc;
} R;

/* ---- helpers ------------------------------------------------------------------------ */

static const char *addr_str(const struct sockaddr_in *a, char *buf, size_t n)
{
    char ip[16];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, n, "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_to(const struct sockaddr_in *to, const uint8_t *data, size_t len)
{
    sendto(R.sock, data, len, 0, (const struct sockaddr *)to, sizeof(*to));
}

static uint32_t crc32_stun(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c ^ 0x5354554Eu;
}

/* ---- STUN encode/decode ------------------------------------------------------------- */

typedef struct {
    uint8_t buf[256];
    size_t len;
} stun_out_t;

static void stun_begin(stun_out_t *m, uint16_t type, const uint8_t txid[12])
{
    m->buf[0] = type >> 8;
    m->buf[1] = type & 0xFF;
    m->buf[2] = m->buf[3] = 0;
    m->buf[4] = 0x21, m->buf[5] = 0x12, m->buf[6] = 0xA4, m->buf[7] = 0x42;
    memcpy(m->buf + 8, txid, 12);
    m->len = 20;
}

static void stun_attr(stun_out_t *m, uint16_t type, const void *v, uint16_t n)
{
    uint8_t *p = m->buf + m->len;
    p[0] = type >> 8, p[1] = type & 0xFF, p[2] = n >> 8, p[3] = n & 0xFF;
    memcpy(p + 4, v, n);
    size_t padded = (n + 3u) & ~3u;
    memset(p + 4 + n, 0, padded - n);
    m->len += 4 + padded;
}

static void stun_set_len(uint8_t *buf, size_t body)
{
    buf[2] = (uint8_t)(body >> 8);
    buf[3] = (uint8_t)body;
}

/* MESSAGE-INTEGRITY with `key` (if any), then FINGERPRINT. */
static void stun_finish(stun_out_t *m, const char *key)
{
    if (key) {
        stun_set_len(m->buf, m->len - 20 + 24);
        uint8_t mac[20];
        mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), (const uint8_t *)key, strlen(key), m->buf,
                        m->len, mac);
        stun_attr(m, ATTR_MESSAGE_INTEGRITY, mac, 20);
    }
    stun_set_len(m->buf, m->len - 20 + 8);
    uint32_t crc = crc32_stun(m->buf, m->len);
    uint8_t c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    stun_attr(m, ATTR_FINGERPRINT, c, 4);
    stun_set_len(m->buf, m->len - 20);
}

/* Find attribute `type`; its offset in *off. */
static const uint8_t *stun_get(const uint8_t *d, size_t len, uint16_t type, uint16_t *n, size_t *off)
{
    size_t p = 20;
    while (p + 4 <= len) {
        uint16_t t = (d[p] << 8) | d[p + 1], l = (d[p + 2] << 8) | d[p + 3];
        if (p + 4 + l > len) return NULL;
        if (t == type) {
            *n = l;
            if (off) *off = p;
            return d + p + 4;
        }
        p += 4 + ((l + 3u) & ~3u);
    }
    return NULL;
}

static bool stun_check_integrity(const uint8_t *d, size_t len, const char *key)
{
    uint16_t n;
    size_t off;
    const uint8_t *mi = stun_get(d, len, ATTR_MESSAGE_INTEGRITY, &n, &off);
    if (!mi || n != 20) return false;
    uint8_t hdr_and_body[600];
    if (off > sizeof(hdr_and_body)) return false;
    memcpy(hdr_and_body, d, off);
    stun_set_len(hdr_and_body, off - 20 + 24);
    uint8_t mac[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), (const uint8_t *)key, strlen(key), hdr_and_body, off,
                    mac);
    return memcmp(mac, mi, 20) == 0;
}

static void send_check(int i)
{
    stun_out_t m;
    uint8_t txid[12];
    port_random(txid, sizeof(txid));
    stun_begin(&m, BINDING_REQUEST, txid);
    char user[140];
    int n = snprintf(user, sizeof(user), "%s:%s", R.remote.ufrag, rtc_identity()->ufrag);
    stun_attr(&m, ATTR_USERNAME, user, (uint16_t)n);
    uint8_t prio[4] = {(HOST_CANDIDATE_PRIORITY >> 24) & 0xFF, (HOST_CANDIDATE_PRIORITY >> 16) & 0xFF, (HOST_CANDIDATE_PRIORITY >> 8) & 0xFF,
                       HOST_CANDIDATE_PRIORITY & 0xFF};
    stun_attr(&m, ATTR_PRIORITY, prio, 4);
    stun_attr(&m, ATTR_ICE_CONTROLLED, R.tiebreaker, 8);
    stun_finish(&m, R.remote.pwd);
    send_to(i < 0 ? &R.peer : &R.cand[i], m.buf, m.len);
}

static void select_pair(const struct sockaddr_in *from, const char *how)
{
    if (R.selected) return;
    R.selected = true;
    R.peer = *from;
    R.fast_until = port_now_ms() + FAST_CHECK_WINDOW_MS;
    R.next_keepalive = port_now_ms() + FAST_CHECK_MS;
    char a[24];
    SLOGI(TAG, "ICE pair selected: %s (%s)", addr_str(from, a, sizeof(a)), how);
}

static void handle_stun(const uint8_t *d, size_t len, const struct sockaddr_in *from)
{
    uint16_t type = (d[0] << 8) | d[1];
    uint16_t n;
    size_t off;
    const uint8_t *fp = stun_get(d, len, ATTR_FINGERPRINT, &n, &off);
    if (fp && n == 4) {
        uint8_t tmp[600];
        if (off > sizeof(tmp)) return;
        memcpy(tmp, d, off);
        stun_set_len(tmp, off - 20 + 8);
        uint32_t want = crc32_stun(tmp, off);
        if (((uint32_t)fp[0] << 24 | fp[1] << 16 | fp[2] << 8 | fp[3]) != want) {
            SLOGW(TAG, "STUN with a bad FINGERPRINT dropped");
            return;
        }
    }
    const rtc_identity_t *id = rtc_identity();
    if (type == BINDING_REQUEST) {
        const uint8_t *user = stun_get(d, len, ATTR_USERNAME, &n, NULL);
        size_t ul = strlen(id->ufrag);
        if (!user || n <= ul || memcmp(user, id->ufrag, ul) != 0 || user[ul] != ':') {
            stun_out_t e;
            stun_begin(&e, BINDING_ERROR, d + 8);
            static const uint8_t unauthorized[] = {0, 0, 4, 1, 'U', 'n', 'a', 'u', 't', 'h', 'o', 'r', 'i', 'z', 'e', 'd'};
            stun_attr(&e, ATTR_ERROR_CODE, unauthorized, sizeof(unauthorized));
            stun_finish(&e, NULL);
            send_to(from, e.buf, e.len);
            SLOGW(TAG, "STUN request with a username for someone else");
            return;
        }
        if (!stun_check_integrity(d, len, id->pwd)) {
            SLOGW(TAG, "STUN request with bad MESSAGE-INTEGRITY dropped");
            return;
        }
        stun_out_t m;
        stun_begin(&m, BINDING_SUCCESS, d + 8);
        uint8_t xa[8] = {0, 1};
        uint16_t xport = ntohs(from->sin_port) ^ (STUN_MAGIC >> 16);
        uint32_t xip = ntohl(from->sin_addr.s_addr) ^ STUN_MAGIC;
        xa[2] = xport >> 8, xa[3] = xport & 0xFF;
        xa[4] = xip >> 24, xa[5] = xip >> 16, xa[6] = xip >> 8, xa[7] = xip;
        stun_attr(&m, ATTR_XOR_MAPPED_ADDRESS, xa, 8);
        stun_finish(&m, id->pwd);
        send_to(from, m.buf, m.len);
        if (!R.have_remote) {
            SLOGW(TAG, "STUN request before the offer arrived");
            return;
        }
        bool first = !R.selected;
        select_pair(from, "incoming binding request");
        if (first && !R.verified_out) {
            send_check(-1);
        }
    } else if (type == BINDING_SUCCESS) {
        if (!R.have_remote) return;
        if (stun_get(d, len, ATTR_MESSAGE_INTEGRITY, &n, NULL) && !stun_check_integrity(d, len, R.remote.pwd)) {
            SLOGW(TAG, "STUN response with bad MESSAGE-INTEGRITY dropped");
            return;
        }
        if (!R.verified_out) {
            char a[24];
            SLOGI(TAG, "ICE check to %s answered", addr_str(from, a, sizeof(a)));
        }
        R.verified_out = true;
        select_pair(from, "binding response");
    } else if (type == BINDING_ERROR) {
        SLOGW(TAG, "STUN error response");
    }
}

/* ---- DTLS --------------------------------------------------------------------------- */

static int dtls_send(void *ctx, const unsigned char *buf, size_t len)
{
    send_to(&R.peer, buf, len);
    return (int)len;
}

static int dtls_recv(void *ctx, unsigned char *buf, size_t len)
{
    if (!R.in_dgram) return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = R.in_len < len ? R.in_len : len;
    memcpy(buf, R.in_dgram, n);
    R.in_dgram = NULL;
    return (int)n;
}

static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    R.timer_start = port_now_ms();
    R.timer_int = int_ms;
    R.timer_fin = fin_ms;
}

static int timer_get(void *ctx)
{
    if (R.timer_fin == 0) return -1;
    int64_t el = port_now_ms() - R.timer_start;
    return el >= R.timer_fin ? 2 : el >= R.timer_int ? 1 : 0;
}

/* The controller's certificate is self-signed: accept it and compare fingerprints after. */
static int verify_any(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    *flags = 0;
    return 0;
}

static void export_keys(void *ctx, mbedtls_ssl_key_export_type type, const unsigned char *secret, size_t len,
                        const unsigned char client_random[32], const unsigned char server_random[32],
                        mbedtls_tls_prf_types prf)
{
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || len != sizeof(R.master)) return;
    memcpy(R.master, secret, len);
    memcpy(R.randbytes, client_random, 32);
    memcpy(R.randbytes + 32, server_random, 32);
    R.prf = prf;
}

static void dtls_reset(void)
{
    if (R.dtls_on) {
        mbedtls_ssl_free(&R.ssl);
        mbedtls_ssl_config_free(&R.conf);
    }
    R.dtls_on = R.dtls_up = R.srtp_ready = false;
    R.timer_fin = 0;
    R.state = DS_NONE;
    R.have_input = false;
}

static bool dtls_start(void)
{
    static const mbedtls_ssl_srtp_profile profiles[] = {MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET};
    mbedtls_ssl_init(&R.ssl);
    mbedtls_ssl_config_init(&R.conf);
    R.dtls_on = true;
    int rc = mbedtls_ssl_config_defaults(&R.conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        mbedtls_ssl_conf_rng(&R.conf, mbedtls_ctr_drbg_random, &R.drbg);
        mbedtls_ssl_conf_authmode(&R.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&R.conf, &R.own_cert, NULL);
        mbedtls_ssl_conf_verify(&R.conf, verify_any, NULL);
        mbedtls_ssl_conf_dtls_cookies(&R.conf, NULL, NULL, NULL); /* no HelloVerifyRequest */
        mbedtls_ssl_conf_max_tls_version(&R.conf, MBEDTLS_SSL_VERSION_TLS1_2);
        rc = mbedtls_ssl_conf_own_cert(&R.conf, &R.own_cert, &R.own_key);
    }
    if (rc == 0) rc = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&R.conf, profiles);
    if (rc == 0) rc = mbedtls_ssl_setup(&R.ssl, &R.conf);
    if (rc != 0) {
        SLOGE(TAG, "DTLS setup failed: -0x%04x", -rc);
        dtls_reset();
        return false;
    }
    mbedtls_ssl_set_bio(&R.ssl, NULL, dtls_send, dtls_recv, NULL);
    mbedtls_ssl_set_timer_cb(&R.ssl, NULL, timer_set, timer_get);
    mbedtls_ssl_set_export_keys_cb(&R.ssl, export_keys, NULL);
    mbedtls_ssl_set_mtu(&R.ssl, 1200);
    SLOGI(TAG, "DTLS start (server)");
    return true;
}

static void on_dtls_connected(void)
{
    R.dtls_up = true;
    R.dtls_at = port_now_ms();
    R.state = DS_DTLS;
    R.next_state_req = R.dtls_at + DISPLAY_RPC_DELAY_MS;
    R.state_reqs = 0;

    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&R.ssl);
    char fp[110] = "(no certificate)";
    if (peer) {
        uint8_t h[32];
        mbedtls_sha256(peer->raw.p, peer->raw.len, h, 0);
        char *p = fp + sprintf(fp, "sha-256 ");
        for (int i = 0; i < 32; i++) p += sprintf(p, i ? ":%02X" : "%02X", h[i]);
    }
    bool fp_ok = strcasecmp(fp, R.remote.fingerprint) == 0 || !R.remote.fingerprint[0];

    mbedtls_dtls_srtp_info srtp;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&R.ssl, &srtp);
    if (srtp.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_UNSET) {
        R.srtp_ready = mbedtls_ssl_tls_prf(R.prf, R.master, sizeof(R.master), "EXTRACTOR-dtls_srtp", R.randbytes,
                                           sizeof(R.randbytes), R.srtp_keys, sizeof(R.srtp_keys)) == 0;
    }
    SLOGI(TAG, "DTLS connected: %s, SRTP %s, peer fingerprint %s", mbedtls_ssl_get_ciphersuite(&R.ssl),
          R.srtp_ready ? "AES128_CM_SHA1_80 keys exported" : "not negotiated", fp_ok ? "verified" : "MISMATCH");
    if (!fp_ok) {
        SLOGW(TAG, "expected %s, got %s", R.remote.fingerprint, fp);
    }
}

/* ---- display RPCs ------------------------------------------------------------------- */

static void set_state(display_state_t s)
{
    if (s > R.state) {
        R.state = s;
        SLOGI(TAG, "display state: %s", DS_NAMES[s]);
    }
}

static void send_wire(uint16_t id, const uint8_t *payload, size_t len)
{
    if (!R.dtls_up) return;
    uint8_t rec[1500];
    size_t total = WIRE_HEADER_LEN + len;
    if (total > sizeof(rec)) return;
    rec[0] = rec[1] = 0;
    rec[2] = id & 0xFF, rec[3] = id >> 8;
    rec[4] = total & 0xFF, rec[5] = (total >> 8) & 0xFF, rec[6] = (total >> 16) & 0xFF;
    rec[7] = 0;
    memcpy(rec + WIRE_HEADER_LEN, payload, len);
    int r = mbedtls_ssl_write(&R.ssl, rec, total);
    if (r < 0) SLOGW(TAG, "DTLS write failed: -0x%04x", -r);
}

/* RpcMessage{request|notification{ticket, payload}} or {response{ticket, code, message, payload?}} */
static void send_rpc(int kind, uint32_t ticket, int code, const char *message, const uint8_t *payload, size_t plen)
{
    uint8_t inner[600], outer[640];
    pbw_t i = PBW(inner, sizeof(inner));
    pbw_uint(&i, 1, ticket);
    if (kind == 2) {
        pbw_uint(&i, 2, (uint64_t)code);
        pbw_str(&i, 3, message ? message : "");
        if (payload) pbw_bytes(&i, 4, payload, plen);
    } else {
        pbw_bytes(&i, 2, payload ? payload : (const uint8_t *)"", payload ? plen : 0);
    }
    pbw_t o = PBW(outer, sizeof(outer));
    pbw_bytes(&o, (uint32_t)kind, i.buf, i.len);
    if (!i.overflow && !o.overflow) send_wire(WIRE_ID_RPC_DISPLAY, o.buf, o.len);
}

static void ask_controller_state(void)
{
    uint8_t msg[8];
    pbw_t w = PBW(msg, sizeof(msg));
    uint8_t req[4];
    pbw_t r = PBW(req, sizeof(req));
    pbw_uint(&r, 2, 0); /* display_supports_send_log_report = false */
    pbw_bytes(&w, D2C_GET_CONTROLLER_STATE, r.buf, r.len);
    R.state_ticket = ++R.ticket;
    send_rpc(1, R.state_ticket, 0, NULL, w.buf, w.len);
    if (R.state_reqs++ < 3) SLOGI(TAG, "GetControllerStateRequest #%u sent", (unsigned)R.state_ticket);
    set_state(DS_ASKED);
}

static int32_t zigzag(uint64_t v)
{
    return (int32_t)((v >> 1) ^ (~(v & 1) + 1));
}

static void on_input(const uint8_t *m, size_t len)
{
    static const char *const NAMES[] = {"Up", "Down", "Left", "Right", "Menu", "Options", "L3", "R3", "L1",
                                        "R1", "Assistant", "Capture", "A", "B", "X", "Y", "Stadia"};
    uint32_t buttons = 0, lt = 0, rt = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    pbf_t f;
    pbr_t r = PBR(m, len);
    while (pbr_next(&r, &f)) {
        switch (f.field) {
        case 1: buttons = (uint32_t)f.v; break;
        case 2: lx = zigzag(f.v); break;
        case 3: ly = zigzag(f.v); break;
        case 4: rx = zigzag(f.v); break;
        case 5: ry = zigzag(f.v); break;
        case 6: lt = (uint32_t)f.v; break;
        case 7: rt = (uint32_t)f.v; break;
        }
    }
    set_state(DS_INPUT);
    uint32_t changed = R.have_input ? buttons ^ R.last_buttons : buttons;
    char line[200] = "";
    size_t n = 0;
    for (int b = 0; b < 17; b++) {
        if (changed & (1u << b)) {
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %s%s", NAMES[b], (buttons & (1u << b)) ? "+" : "-");
        }
    }
    R.last_buttons = buttons;
    R.have_input = true;
    SLOGI(TAG, "input:%s L(%d,%d) R(%d,%d) L2 %u R2 %u", n ? line : " (sticks)", (int)lx, (int)ly, (int)rx, (int)ry,
          (unsigned)lt, (unsigned)rt);
}

static void on_rpc(const uint8_t *data, size_t len)
{
    pbf_t body;
    pbr_t r = PBR(data, len);
    if (!pbr_next(&r, &body) || body.wt != PB_LEN) {
        SLOGW(TAG, "undecodable RpcMessage (%u bytes)", (unsigned)len);
        return;
    }
    int kind = (int)body.field; /* 1 request, 2 response, 3 notification */
    uint32_t ticket = 0;
    bool has_ticket = false;
    uint64_t code = 0;
    pbf_t f, payload = {0};
    r = PBR(body.data, body.len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) ticket = (uint32_t)f.v, has_ticket = true;
        if (kind == 2 && f.field == 2) code = f.v;
        if ((kind == 2 && f.field == 4) || (kind != 2 && f.field == 2)) payload = f;
    }
    pbf_t variant = {0};
    if (payload.data) {
        pbr_t pr = PBR(payload.data, payload.len);
        pbr_next(&pr, &variant);
    }

    if (kind == 1) { /* the controller's requests */
        if (!has_ticket) return;
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
            send_rpc(2, ticket, RPC_OK, "", m.buf, m.len);
            break;
        }
        case C2D_KEEPALIVE:
            set_state(DS_READY);
            send_rpc(2, ticket, RPC_OK, "", NULL, 0);
            break;
        case C2D_DISCONNECT:
            SLOGI(TAG, "the controller asks to disconnect");
            send_rpc(2, ticket, RPC_OK, "", NULL, 0);
            break;
        default:
            SLOGI(TAG, "request #%u (ControllerToDisplay #%u): answered UNIMPLEMENTED", (unsigned)ticket, (unsigned)variant.field);
            send_rpc(2, ticket, RPC_UNIMPLEMENTED, "Unimplemented RPC in local receiver", NULL, 0);
        }
    } else if (kind == 3) { /* notifications */
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
    } else if (kind == 2 && ticket == R.state_ticket && R.state < DS_STATE) {
        if (code != 0 || variant.field != C2D_CONTROLLER_STATE) {
            if (R.state_reqs <= 3) SLOGI(TAG, "GetControllerState answered with code %u, asking again", (unsigned)code);
            return;
        }
        char product[24] = "", hw[16] = "", fw[16] = "";
        bool headset = false;
        r = PBR(variant.data, variant.len);
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

/* Input as RTP on sub-channel 0x66 (the fake controller, and the game session in step 3):
 * ack InputProtocolMessage{ack_sequence_number} the same way. */
static void on_wire_rtp(const uint8_t *p, size_t len)
{
    if (len < 12 || (p[0] >> 6) != 2 || (p[1] >= 192 && p[1] <= 223)) return; /* not RTP, or RTCP */
    uint8_t pt = p[1] & 0x7F;
    size_t off = 12 + 4u * (p[0] & 0x0F);
    if (p[0] & 0x10) {
        if (off + 4 > len) return;
        off += 4 + 4u * ((p[off + 2] << 8) | p[off + 3]);
    }
    size_t end = len;
    if ((p[0] & 0x20) && len > 0) end -= p[len - 1];
    if (off > end) return;
    pbf_t series, seq;
    if (!pb_find(p + off, end - off, 7, &series) || !pb_find(series.data, series.len, 2, &seq)) {
        return; /* compressed or not a series: step 3 decodes those */
    }
    set_state(DS_INPUT);
    SLOGI(TAG, "input series #%llu on 0x66 (%u bytes)", (unsigned long long)seq.v, (unsigned)(end - off));
    uint8_t ack[16];
    pbw_t a = PBW(ack, sizeof(ack));
    pbw_uint(&a, 4, seq.v);
    uint8_t pkt[12 + 16];
    uint32_t ts = (uint32_t)(port_now_ms() * 90);
    R.tx_seq++;
    pkt[0] = 0x80, pkt[1] = pt;
    pkt[2] = R.tx_seq >> 8, pkt[3] = R.tx_seq & 0xFF;
    pkt[4] = ts >> 24, pkt[5] = ts >> 16, pkt[6] = ts >> 8, pkt[7] = ts;
    pkt[8] = R.ssrc >> 24, pkt[9] = R.ssrc >> 16, pkt[10] = R.ssrc >> 8, pkt[11] = R.ssrc;
    memcpy(pkt + 12, a.buf, a.len);
    send_wire(WIRE_ID_RTP, pkt, 12 + a.len);
}

static void on_app_data(const uint8_t *rec, size_t len)
{
    if (len < WIRE_HEADER_LEN || (rec[0] >> 4) != 0) return;
    uint16_t id = rec[2] | (rec[3] << 8);
    size_t total = rec[4] | (rec[5] << 8) | ((size_t)rec[6] << 16);
    if (total != len) {
        SLOGW(TAG, "WireHeader2 length %u != record %u", (unsigned)total, (unsigned)len);
        return;
    }
    if (id == WIRE_ID_RTP) {
        on_wire_rtp(rec + WIRE_HEADER_LEN, len - WIRE_HEADER_LEN);
    } else if (id == WIRE_ID_RPC_CONTROLLER || id == WIRE_ID_RPC_DISPLAY) {
        on_rpc(rec + WIRE_HEADER_LEN, len - WIRE_HEADER_LEN);
    } else {
        SLOGI(TAG, "WireHeader2 id 0x%04x, %u bytes", id, (unsigned)len);
    }
}

static void dtls_pump(const uint8_t *dgram, size_t len)
{
    R.in_dgram = dgram;
    R.in_len = len;
    if (!R.dtls_up) {
        int rc = mbedtls_ssl_handshake(&R.ssl);
        if (rc == 0) {
            on_dtls_connected();
        } else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char why[100];
            mbedtls_strerror(rc, why, sizeof(why));
            SLOGW(TAG, "DTLS handshake failed: %s", why);
            dtls_reset();
            return;
        }
    }
    while (R.dtls_up) {
        uint8_t buf[1500];
        int r = mbedtls_ssl_read(&R.ssl, buf, sizeof(buf));
        if (r > 0) {
            on_app_data(buf, (size_t)r);
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            SLOGI(TAG, "the controller closed the link (powering off)");
            dtls_reset();
            R.selected = R.verified_out = R.have_remote = false;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE && r != 0) {
            SLOGW(TAG, "DTLS read: -0x%04x", -r);
        }
        break;
    }
    R.in_dgram = NULL;
}

/* ---- the loop ----------------------------------------------------------------------- */

static void take_remote(void)
{
    port_lock();
    bool fresh = R.remote_new;
    R.remote_new = false;
    port_unlock();
    if (!fresh) return;
    dtls_reset();
    R.selected = R.verified_out = false;
    R.have_remote = true;
    R.ncand = 0;
    for (int i = 0; i < R.remote.candidates && i < 4; i++) {
        struct sockaddr_in *c = &R.cand[R.ncand];
        memset(c, 0, sizeof(*c));
        c->sin_family = AF_INET;
        c->sin_port = htons(R.remote.cand_port[i]);
        if (inet_pton(AF_INET, R.remote.cand_ip[i], &c->sin_addr) == 1) {
            R.check_attempts[R.ncand] = 0;
            R.next_check[R.ncand] = port_now_ms();
            R.ncand++;
        }
    }
    SLOGI(TAG, "remote description set: ufrag %s, %d candidate(s)", R.remote.ufrag, R.ncand);
}

static void tick(void)
{
    int64_t now = port_now_ms();
    /* our checks to the offered candidates: retried at 0.5 s, 1 s, 1.5 s ... until answered */
    if (R.have_remote && !R.verified_out) {
        for (int i = 0; i < R.ncand; i++) {
            if (R.check_attempts[i] < 8 && now >= R.next_check[i]) {
                send_check(i);
                R.check_attempts[i]++;
                R.next_check[i] = now + 500 * R.check_attempts[i];
            }
        }
    }
    /* keepalive checks on the selected pair: every 1 s until DTLS (the controller's ClientHello
     * follows one of our checks), every 2.5 s after */
    if (R.selected && now >= R.next_keepalive) {
        send_check(-1);
        bool fast = !R.dtls_on && now < R.fast_until;
        R.next_keepalive = now + (fast ? FAST_CHECK_MS : KEEPALIVE_MS);
    }
    /* DTLS retransmission */
    if (R.dtls_on && !R.dtls_up && timer_get(NULL) == 2) {
        dtls_pump(NULL, 0);
    }
    /* GetControllerStateRequest until answered */
    if (R.dtls_up && R.state < DS_STATE && now >= R.next_state_req && now - R.dtls_at < DISPLAY_RPC_WINDOW_MS) {
        ask_controller_state();
        R.next_state_req = now + DISPLAY_RPC_INTERVAL_MS;
    }
}

static void rtc_task(void *arg)
{
    uint8_t buf[1600];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(R.sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        take_remote();
        if (n >= 2) {
            uint8_t b = buf[0];
            if (b < 4 && n >= 20 && buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42) {
                handle_stun(buf, (size_t)n, &from);
            } else if (b >= 20 && b <= 63) {
                if (!R.dtls_on) {
                    if (b != 22 || !R.have_remote) {
                        SLOGW(TAG, "DTLS record without a session (content type %u)", b);
                        goto tick;
                    }
                    if (!R.selected) select_pair(&from, "DTLS before STUN");
                    if (!dtls_start()) goto tick;
                }
                if (same_addr(&from, &R.peer)) dtls_pump(buf, (size_t)n);
            }
        }
    tick:
        tick();
    }
}

void rtc_set_remote(const remote_offer_t *remote)
{
    port_lock();
    R.remote = *remote;
    R.remote_new = true;
    port_unlock();
}

bool rtc_start(uint16_t port)
{
    const rtc_identity_t *id = rtc_identity();
    mbedtls_x509_crt_init(&R.own_cert);
    mbedtls_pk_init(&R.own_key);
    mbedtls_entropy_init(&R.entropy);
    mbedtls_ctr_drbg_init(&R.drbg);
    int rc = mbedtls_ctr_drbg_seed(&R.drbg, mbedtls_entropy_func, &R.entropy, (const unsigned char *)"dtls", 4);
    if (rc == 0) rc = mbedtls_x509_crt_parse_der(&R.own_cert, id->cert_der, id->cert_der_len);
    if (rc == 0) {
        rc = mbedtls_pk_parse_key(&R.own_key, id->key_der, id->key_der_len, NULL, 0, mbedtls_ctr_drbg_random, &R.drbg);
    }
    if (rc != 0) {
        SLOGE(TAG, "DTLS identity: -0x%04x", -rc);
        return false;
    }
    port_random(R.tiebreaker, sizeof(R.tiebreaker));
    port_random(&R.ssrc, sizeof(R.ssrc));
    R.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (R.sock < 0 || bind(R.sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SLOGE(TAG, "cannot bind UDP %u", port);
        return false;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50 * 1000};
    setsockopt(R.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    SLOGI(TAG, "display link on UDP %u", port);
    return port_spawn(rtc_task, NULL, "rtc", 16 * 1024);
}
