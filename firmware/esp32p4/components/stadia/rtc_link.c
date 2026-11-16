/*
 * ICE, DTLS and WireHeader2 for one link (see rtc_link.h).
 *
 *   ICE      answer the controller's STUN binding requests, send our own checks (we are the
 *            controlled agent), every 1 s until DTLS starts and every 2.5 s after
 *   DTLS     server role (the live preset's dtls_role=server), use_srtp, SRTP keys exported
 */
#include "rtc_link.h"

#include "mbedtls/private_access.h"
#include "stadia_pb.h"
#include "stadia_port.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "rtc";

#define STUN_MAGIC 0x2112A442u
#define BINDING_REQUEST 0x0001
#define BINDING_SUCCESS 0x0101
#define BINDING_ERROR 0x0111
#define ATTR_USERNAME 0x0006
#define ATTR_MESSAGE_INTEGRITY 0x0008
#define ATTR_ERROR_CODE 0x0009
#define ATTR_XOR_MAPPED_ADDRESS 0x0020
#define ATTR_PRIORITY 0x0024
#define ATTR_FINGERPRINT 0x8028
#define ATTR_ICE_CONTROLLED 0x8029
#define HOST_CANDIDATE_PRIORITY 2130706431u

#define FAST_CHECK_MS 1000
#define FAST_CHECK_WINDOW_MS 30000
#define KEEPALIVE_MS 2500
/* A connected peer answers our keepalive checks every 2.5 s, so this much silence means the
 * controller went to sleep or lost power without a close_notify: close the link, which
 * releases the gamepad, instead of holding its last state until it comes back. */
#define LINK_SILENCE_MS 10000

static struct {
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt own_cert;
    mbedtls_pk_context own_key;
} G;

/* ---- helpers ------------------------------------------------------------------------ */

static const char *addr_str(const struct sockaddr_in *a, char *buf, size_t n)
{
    char ip[16];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, n, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
    return buf;
}

static bool same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void send_to(rtc_link_t *L, const struct sockaddr_in *to, const uint8_t *data, size_t len)
{
    sendto(L->sock, data, len, 0, (const struct sockaddr *)to, sizeof(*to));
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
    uint8_t c[4] = {(crc >> 24) & 0xFF, (crc >> 16) & 0xFF, (crc >> 8) & 0xFF, crc & 0xFF};
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

static void send_check(rtc_link_t *L, int i)
{
    stun_out_t m;
    uint8_t txid[12];
    port_random(txid, sizeof(txid));
    stun_begin(&m, BINDING_REQUEST, txid);
    char user[140];
    int n = snprintf(user, sizeof(user), "%s:%s", L->remote.ufrag, L->ufrag);
    stun_attr(&m, ATTR_USERNAME, user, (uint16_t)n);
    uint8_t prio[4] = {(HOST_CANDIDATE_PRIORITY >> 24) & 0xFF, (HOST_CANDIDATE_PRIORITY >> 16) & 0xFF,
                       (HOST_CANDIDATE_PRIORITY >> 8) & 0xFF, HOST_CANDIDATE_PRIORITY & 0xFF};
    stun_attr(&m, ATTR_PRIORITY, prio, 4);
    stun_attr(&m, ATTR_ICE_CONTROLLED, L->tiebreaker, 8);
    stun_finish(&m, L->remote.pwd);
    send_to(L, i < 0 ? &L->peer : &L->cand[i], m.buf, m.len);
    if (i < 0 && L->dtls_up) { /* a keepalive: timed; one still unanswered counts as lost */
        memcpy(L->ping_txid, txid, sizeof(txid));
        L->ping_sent_us = port_now_us();
        L->pings_sent++;
    }
}

static void select_pair(rtc_link_t *L, const struct sockaddr_in *from, const char *how)
{
    if (L->selected) return;
    L->selected = true;
    L->peer = *from;
    L->fast_until = port_now_ms() + FAST_CHECK_WINDOW_MS;
    L->next_keepalive = port_now_ms() + FAST_CHECK_MS;
    char a[24];
    SLOGI(TAG, "%s: ICE pair selected: %s (%s)", L->name, addr_str(from, a, sizeof(a)), how);
}

static void handle_stun(rtc_link_t *L, const uint8_t *d, size_t len, const struct sockaddr_in *from)
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
            SLOGW(TAG, "%s: STUN with a bad FINGERPRINT dropped", L->name);
            return;
        }
    }
    if (type == BINDING_REQUEST) {
        const uint8_t *user = stun_get(d, len, ATTR_USERNAME, &n, NULL);
        size_t ul = strlen(L->ufrag);
        if (!user || n <= ul || memcmp(user, L->ufrag, ul) != 0 || user[ul] != ':') {
            stun_out_t e;
            stun_begin(&e, BINDING_ERROR, d + 8);
            static const uint8_t unauthorized[] = {0, 0, 4, 1, 'U', 'n', 'a', 'u', 't', 'h', 'o', 'r', 'i', 'z', 'e', 'd'};
            stun_attr(&e, ATTR_ERROR_CODE, unauthorized, sizeof(unauthorized));
            stun_finish(&e, NULL);
            send_to(L, from, e.buf, e.len);
            SLOGW(TAG, "%s: STUN request with a username for someone else", L->name);
            return;
        }
        if (!stun_check_integrity(d, len, L->pwd)) {
            SLOGW(TAG, "%s: STUN request with bad MESSAGE-INTEGRITY dropped", L->name);
            return;
        }
        stun_out_t m;
        stun_begin(&m, BINDING_SUCCESS, d + 8);
        uint8_t xa[8] = {0, 1};
        uint16_t xport = ntohs(from->sin_port) ^ (STUN_MAGIC >> 16);
        uint32_t xip = ntohl(from->sin_addr.s_addr) ^ STUN_MAGIC;
        xa[2] = xport >> 8, xa[3] = xport & 0xFF;
        xa[4] = xip >> 24, xa[5] = (xip >> 16) & 0xFF, xa[6] = (xip >> 8) & 0xFF, xa[7] = xip & 0xFF;
        stun_attr(&m, ATTR_XOR_MAPPED_ADDRESS, xa, 8);
        stun_finish(&m, L->pwd);
        send_to(L, from, m.buf, m.len);
        if (!L->have_remote) {
            SLOGW(TAG, "%s: STUN request before the offer arrived", L->name);
            return;
        }
        bool first = !L->selected;
        select_pair(L, from, "incoming binding request");
        if (first && !L->verified_out) {
            send_check(L, -1);
        }
    } else if (type == BINDING_SUCCESS) {
        if (!L->have_remote) return;
        if (stun_get(d, len, ATTR_MESSAGE_INTEGRITY, &n, NULL) && !stun_check_integrity(d, len, L->remote.pwd)) {
            SLOGW(TAG, "%s: STUN response with bad MESSAGE-INTEGRITY dropped", L->name);
            return;
        }
        if (!L->verified_out) {
            char a[24];
            SLOGI(TAG, "%s: ICE check to %s answered", L->name, addr_str(from, a, sizeof(a)));
        }
        L->verified_out = true;
        if (L->ping_sent_us && memcmp(d + 8, L->ping_txid, 12) == 0) {
            int64_t rtt = port_now_us() - L->ping_sent_us;
            L->ping_us = rtt > INT32_MAX ? INT32_MAX : (int32_t)rtt;
            L->ping_sent_us = 0;
            L->pings_answered++;
        }
        select_pair(L, from, "binding response");
    } else if (type == BINDING_ERROR) {
        SLOGW(TAG, "%s: STUN error response", L->name);
    }
}

/* ---- DTLS --------------------------------------------------------------------------- */

static int dtls_send(void *ctx, const unsigned char *buf, size_t len)
{
    rtc_link_t *L = ctx;
    send_to(L, &L->peer, buf, len);
    return (int)len;
}

static int dtls_recv(void *ctx, unsigned char *buf, size_t len)
{
    rtc_link_t *L = ctx;
    if (!L->in_dgram) return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = L->in_len < len ? L->in_len : len;
    memcpy(buf, L->in_dgram, n);
    L->in_dgram = NULL;
    return (int)n;
}

static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    rtc_link_t *L = ctx;
    L->timer_start = port_now_ms();
    L->timer_int = int_ms;
    L->timer_fin = fin_ms;
}

static int timer_get(void *ctx)
{
    rtc_link_t *L = ctx;
    if (L->timer_fin == 0) return -1;
    int64_t el = port_now_ms() - L->timer_start;
    return el >= L->timer_fin ? 2 : el >= L->timer_int ? 1 : 0;
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
    rtc_link_t *L = ctx;
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || len != sizeof(L->master)) return;
    memcpy(L->master, secret, len);
    memcpy(L->randbytes, client_random, 32);
    memcpy(L->randbytes + 32, server_random, 32);
    L->prf = prf;
}

static void dtls_reset(rtc_link_t *L)
{
    bool was_up = L->dtls_up;
    if (L->dtls_on) {
        mbedtls_ssl_free(&L->ssl);
        mbedtls_ssl_config_free(&L->conf);
    }
    L->dtls_on = L->dtls_up = L->srtp_ready = false;
    srtp_tx_free(&L->srtp_out);
    srtp_rx_free(&L->srtp_in);
    L->in_ssrc = 0;
    L->media_in = L->rtcp_in = L->srtp_fail = 0;
    if (L->ping_sent_us) L->pings_sent--; /* still in flight: neither answered nor lost */
    L->ping_sent_us = 0;
    L->ping_us = -1;
    L->timer_fin = 0;
    if (was_up && L->ops->on_closed) L->ops->on_closed(L);
}

static bool dtls_start(rtc_link_t *L)
{
    static const mbedtls_ssl_srtp_profile profiles[] = {MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET};
    mbedtls_ssl_init(&L->ssl);
    mbedtls_ssl_config_init(&L->conf);
    L->dtls_on = true;
    int rc = mbedtls_ssl_config_defaults(&L->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        mbedtls_ssl_conf_rng(&L->conf, mbedtls_ctr_drbg_random, &G.drbg);
        mbedtls_ssl_conf_authmode(&L->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&L->conf, &G.own_cert, NULL);
        mbedtls_ssl_conf_verify(&L->conf, verify_any, NULL);
        mbedtls_ssl_conf_dtls_cookies(&L->conf, NULL, NULL, NULL); /* no HelloVerifyRequest */
        mbedtls_ssl_conf_max_tls_version(&L->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        rc = mbedtls_ssl_conf_own_cert(&L->conf, &G.own_cert, &G.own_key);
    }
    if (rc == 0) rc = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&L->conf, profiles);
    if (rc == 0) rc = mbedtls_ssl_setup(&L->ssl, &L->conf);
    if (rc != 0) {
        SLOGE(TAG, "%s: DTLS setup failed: -0x%04x", L->name, (unsigned)-rc);
        dtls_reset(L);
        return false;
    }
    mbedtls_ssl_set_bio(&L->ssl, L, dtls_send, dtls_recv, NULL);
    mbedtls_ssl_set_timer_cb(&L->ssl, L, timer_set, timer_get);
    mbedtls_ssl_set_export_keys_cb(&L->ssl, export_keys, L);
    mbedtls_ssl_set_mtu(&L->ssl, 1200);
    SLOGI(TAG, "%s: DTLS start (server)", L->name);
    return true;
}

static void on_dtls_connected(rtc_link_t *L)
{
    L->dtls_up = true;
    L->dtls_at = port_now_ms();

    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&L->ssl);
    char fp[110] = "(no certificate)";
    if (peer) {
        uint8_t h[32];
        mbedtls_sha256(peer->raw.p, peer->raw.len, h, 0);
        char *p = fp + sprintf(fp, "sha-256 ");
        for (int i = 0; i < 32; i++) p += sprintf(p, i ? ":%02X" : "%02X", h[i]);
    }
    bool fp_ok = strcasecmp(fp, L->remote.fingerprint) == 0 || !L->remote.fingerprint[0];

    mbedtls_dtls_srtp_info srtp;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&L->ssl, &srtp);
    if (srtp.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_UNSET) {
        L->srtp_ready = mbedtls_ssl_tls_prf(L->prf, L->master, sizeof(L->master), "EXTRACTOR-dtls_srtp", L->randbytes,
                                            sizeof(L->randbytes), L->srtp_keys, sizeof(L->srtp_keys)) == 0;
        /* we are the DTLS server: our packets use the server write key (16..32) and salt (46..60) */
        if (L->srtp_ready) srtp_tx_init(&L->srtp_out, L->srtp_keys + 16, L->srtp_keys + 46);
        /* and the controller's media (its microphone) the client write key (0..16) and salt (32..46) */
        if (L->srtp_ready) srtp_rx_init(&L->srtp_in, L->srtp_keys, L->srtp_keys + 32);
    }
    SLOGI(TAG, "%s: DTLS connected: %s, SRTP %s, peer fingerprint %s", L->name, mbedtls_ssl_get_ciphersuite(&L->ssl),
          L->srtp_ready ? "AES128_CM_SHA1_80 keys exported" : "not negotiated", fp_ok ? "verified" : "MISMATCH");
    if (!fp_ok) {
        SLOGW(TAG, "%s: expected %s, got %s", L->name, L->remote.fingerprint, fp);
    }
    if (L->ops->on_connected) L->ops->on_connected(L);
}

static void on_record(rtc_link_t *L, const uint8_t *rec, size_t len)
{
    if (len >= WIRE_HEADER_LEN && (rec[0] >> 4) == 0) {
        uint16_t id = rec[2] | (rec[3] << 8);
        size_t total = rec[4] | (rec[5] << 8) | ((size_t)rec[6] << 16);
        if (total == len) {
            L->ops->on_wire(L, id, rec + WIRE_HEADER_LEN, len - WIRE_HEADER_LEN);
            return;
        }
    }
    if (L->ops->on_unframed) {
        L->ops->on_unframed(L, rec, len);
    } else {
        SLOGW(TAG, "%s: DTLS record without WireHeader2 (%u bytes)", L->name, (unsigned)len);
    }
}

static void dtls_pump(rtc_link_t *L, const uint8_t *dgram, size_t len)
{
    L->in_dgram = dgram;
    L->in_len = len;
    if (!L->dtls_up) {
        int rc = mbedtls_ssl_handshake(&L->ssl);
        if (rc == 0) {
            on_dtls_connected(L);
        } else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char why[100];
            mbedtls_strerror(rc, why, sizeof(why));
            SLOGW(TAG, "%s: DTLS handshake failed: %s", L->name, why);
            dtls_reset(L);
            return;
        }
    }
    while (L->dtls_up) {
        uint8_t buf[1500];
        int r = mbedtls_ssl_read(&L->ssl, buf, sizeof(buf));
        if (r > 0) {
            on_record(L, buf, (size_t)r);
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            SLOGI(TAG, "%s: the controller closed the link", L->name);
            dtls_reset(L);
            L->selected = L->verified_out = L->have_remote = false;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE && r != 0) {
            SLOGW(TAG, "%s: DTLS read: -0x%04x", L->name, (unsigned)-r);
        }
        break;
    }
    L->in_dgram = NULL;
}

/* ---- sending ------------------------------------------------------------------------ */

bool rtc_link_up(const rtc_link_t *L)
{
    return L->dtls_up;
}

bool rtc_link_send_record(rtc_link_t *L, const uint8_t *payload, size_t len)
{
    if (!L->dtls_up) return false;
    int r = mbedtls_ssl_write(&L->ssl, payload, len);
    if (r < 0) {
        SLOGW(TAG, "%s: DTLS write failed: -0x%04x", L->name, (unsigned)-r);
        return false;
    }
    return true;
}

bool rtc_link_send_srtp(rtc_link_t *L, uint8_t *pkt, size_t len, size_t cap)
{
    if (!L->dtls_up || !L->srtp_ready || !L->selected) return false;
    size_t n = srtp_protect(&L->srtp_out, pkt, len, cap);
    if (!n) return false;
    send_to(L, &L->peer, pkt, n);
    return true;
}

bool rtc_link_send_wire(rtc_link_t *L, uint16_t id, const uint8_t *payload, size_t len)
{
    uint8_t rec[1400];
    size_t total = WIRE_HEADER_LEN + len;
    if (total > sizeof(rec)) return false;
    rec[0] = rec[1] = 0;
    rec[2] = id & 0xFF, rec[3] = id >> 8;
    rec[4] = total & 0xFF, rec[5] = (total >> 8) & 0xFF, rec[6] = (total >> 16) & 0xFF;
    rec[7] = 0;
    memcpy(rec + WIRE_HEADER_LEN, payload, len);
    return rtc_link_send_record(L, rec, total);
}

bool rtc_link_send_rtp_wire(rtc_link_t *L, uint8_t pt, const uint8_t *payload, size_t len)
{
    uint8_t pkt[1300];
    if (12 + len > sizeof(pkt)) return false;
    uint32_t ts = (uint32_t)(port_now_ms() * 90);
    L->tx_seq++;
    pkt[0] = 0x80, pkt[1] = pt & 0x7F;
    pkt[2] = L->tx_seq >> 8, pkt[3] = L->tx_seq & 0xFF;
    pkt[4] = ts >> 24, pkt[5] = (ts >> 16) & 0xFF, pkt[6] = (ts >> 8) & 0xFF, pkt[7] = ts & 0xFF;
    pkt[8] = L->ssrc >> 24, pkt[9] = (L->ssrc >> 16) & 0xFF, pkt[10] = (L->ssrc >> 8) & 0xFF, pkt[11] = L->ssrc & 0xFF;
    memcpy(pkt + 12, payload, len);
    return rtc_link_send_wire(L, WIRE_ID_RTP, pkt, 12 + len);
}

bool rtp_payload(const uint8_t *p, size_t len, uint8_t *pt, const uint8_t **payload, size_t *plen)
{
    if (len < 12 || (p[0] >> 6) != 2 || (p[1] >= 192 && p[1] <= 223)) return false; /* not RTP, or RTCP */
    size_t off = 12 + 4u * (p[0] & 0x0F);
    if (p[0] & 0x10) {
        if (off + 4 > len) return false;
        off += 4 + 4u * ((p[off + 2] << 8) | p[off + 3]);
    }
    size_t end = len;
    if ((p[0] & 0x20) && len > 0) {
        if (p[len - 1] > len) return false;
        end -= p[len - 1];
    }
    if (off > end) return false;
    *pt = p[1] & 0x7F;
    *payload = p + off;
    *plen = end - off;
    return true;
}

/* ---- RPC messages ------------------------------------------------------------------- */

size_t rpc_encode(int kind, uint32_t ticket, int code, const char *message, const uint8_t *payload, size_t plen,
                  uint8_t *out, size_t cap)
{
    uint8_t inner[1100];
    pbw_t i = PBW(inner, sizeof(inner));
    pbw_uint(&i, 1, ticket);
    if (kind == 2) {
        pbw_uint(&i, 2, (uint64_t)code);
        pbw_str(&i, 3, message ? message : "");
        if (payload) pbw_bytes(&i, 4, payload, plen);
    } else {
        pbw_bytes(&i, 2, payload ? payload : (const uint8_t *)"", payload ? plen : 0);
    }
    pbw_t o = PBW(out, cap);
    pbw_bytes(&o, (uint32_t)kind, i.buf, i.len);
    return (i.overflow || o.overflow) ? 0 : o.len;
}

bool rpc_decode(const uint8_t *data, size_t len, rpc_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    pbf_t body, f;
    pbr_t r = PBR(data, len);
    if (!pbr_next(&r, &body) || body.wt != PB_LEN || body.field < 1 || body.field > 3) return false;
    m->kind = (int)body.field;
    r = PBR(body.data, body.len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1) m->ticket = (uint32_t)f.v, m->has_ticket = true;
        if (m->kind == 2 && f.field == 2) m->code = f.v;
        if ((m->kind == 2 && f.field == 4) || (m->kind != 2 && f.field == 2)) {
            m->payload = f.data;
            m->plen = f.len;
        }
    }
    return !r.error;
}

/* ---- the link ----------------------------------------------------------------------- */

static void take_remote(rtc_link_t *L)
{
    port_lock();
    bool fresh = L->remote_new;
    if (fresh) L->remote = L->pending;
    L->remote_new = false;
    port_unlock();
    if (!fresh) return;
    dtls_reset(L);
    L->selected = L->verified_out = false;
    L->have_remote = true;
    L->ncand = 0;
    for (int i = 0; i < L->remote.candidates && i < 4; i++) {
        struct sockaddr_in *c = &L->cand[L->ncand];
        memset(c, 0, sizeof(*c));
        c->sin_family = AF_INET;
        c->sin_port = htons(L->remote.cand_port[i]);
        if (inet_pton(AF_INET, L->remote.cand_ip[i], &c->sin_addr) == 1) {
            L->check_attempts[L->ncand] = 0;
            L->next_check[L->ncand] = port_now_ms();
            L->ncand++;
        }
    }
    SLOGI(TAG, "%s: remote description set: ufrag %s, %d candidate(s)", L->name, L->remote.ufrag, L->ncand);
}

/* SRTP / SRTCP from the controller (RFC 7983: first byte 128..191): the media it sends besides
 * the DTLS records, its microphone. Nothing like it is documented, so every new SSRC is logged
 * with its payload type and size, and SRTCP is only counted. The buffer is the task's own, so
 * the RTP is decrypted in place. */
static void handle_media(rtc_link_t *L, const uint8_t *cbuf, size_t n, const struct sockaddr_in *from)
{
    uint8_t *buf = (uint8_t *)cbuf;
    bool rtcp = n >= 2 && buf[1] >= 192 && buf[1] <= 223;
    if (!L->selected || !same_addr(from, &L->peer)) {
        if (L->stray_logged++ < 3) {
            SLOGI(TAG, "%s: %s from a peer that is not the selected pair (%u bytes)", L->name, rtcp ? "RTCP" : "RTP",
                  (unsigned)n);
        }
        return;
    }
    if (rtcp) {
        if (L->rtcp_in++ < 3) {
            SLOGI(TAG, "%s: SRTCP from the controller: PT %u, %u bytes, SSRC %02x%02x%02x%02x", L->name, buf[1],
                  (unsigned)n, buf[4], buf[5], buf[6], buf[7]);
        }
        return;
    }
    if (n < 12 || !L->srtp_ready) return;
    uint32_t ssrc = (uint32_t)buf[8] << 24 | (uint32_t)buf[9] << 16 | (uint32_t)buf[10] << 8 | buf[11];
    if (ssrc != L->in_ssrc) { /* a new stream: its own rollover counter */
        SLOGI(TAG, "%s: media from the controller: SSRC %08x, PT %u, seq %u, %u bytes", L->name, (unsigned)ssrc,
              buf[1] & 0x7F, (unsigned)(buf[2] << 8 | buf[3]), (unsigned)n);
        L->in_ssrc = ssrc;
        L->srtp_in.roc = 0;
        L->srtp_in.last_seq = -1;
    }
    size_t len = srtp_unprotect(&L->srtp_in, buf, n);
    if (!len) {
        if (L->srtp_fail++ < 5) {
            SLOGW(TAG, "%s: SRTP from the controller fails authentication (SSRC %08x, %u bytes)", L->name,
                  (unsigned)ssrc, (unsigned)n);
        }
        return;
    }
    L->media_in++;
    if (L->ops->on_media) L->ops->on_media(L, buf, len);
}

void rtc_link_poll(rtc_link_t *L, const uint8_t *buf, size_t n, const struct sockaddr_in *from)
{
    take_remote(L);
    if (n < 2) return;
    if (L->selected && same_addr(from, &L->peer)) L->last_rx = port_now_ms();
    uint8_t b = buf[0];
    if (b < 4 && n >= 20 && buf[4] == 0x21 && buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42) {
        handle_stun(L, buf, n, from);
    } else if (b >= 20 && b <= 63) {
        if (!L->dtls_on) {
            if (b != 22 || !L->have_remote) {
                SLOGW(TAG, "%s: DTLS record without a session (content type %u)", L->name, b);
                return;
            }
            if (!L->selected) select_pair(L, from, "DTLS before STUN");
            if (!dtls_start(L)) return;
        }
        if (same_addr(from, &L->peer)) dtls_pump(L, buf, n);
    } else if (b >= 128 && b <= 191) {
        handle_media(L, buf, n, from);
    }
}

void rtc_link_tick(rtc_link_t *L)
{
    take_remote(L);
    int64_t now = port_now_ms();
    /* our checks to the offered candidates: retried at 0.5 s, 1 s, 1.5 s ... until answered */
    if (L->have_remote && !L->verified_out) {
        for (int i = 0; i < L->ncand; i++) {
            if (L->check_attempts[i] < 8 && now >= L->next_check[i]) {
                send_check(L, i);
                L->check_attempts[i]++;
                L->next_check[i] = now + 500 * L->check_attempts[i];
            }
        }
    }
    /* keepalive checks on the selected pair: every 1 s until DTLS (the controller's ClientHello
     * follows one of our checks), every 2.5 s after */
    if (L->selected && now >= L->next_keepalive) {
        send_check(L, -1);
        bool fast = !L->dtls_on && now < L->fast_until;
        L->next_keepalive = now + (fast ? FAST_CHECK_MS : KEEPALIVE_MS);
    }
    if (L->dtls_up) {
        int64_t heard = L->last_rx > L->dtls_at ? L->last_rx : L->dtls_at;
        if (now - heard > LINK_SILENCE_MS) {
            SLOGW(TAG, "%s: nothing from the controller for %d s: closing the link", L->name, LINK_SILENCE_MS / 1000);
            dtls_reset(L);
            L->selected = L->verified_out = L->have_remote = false;
            return;
        }
    }
    /* DTLS retransmission */
    if (L->dtls_on && !L->dtls_up && timer_get(L) == 2) {
        dtls_pump(L, NULL, 0);
    }
}

void rtc_link_set_credentials(rtc_link_t *L, const char *ufrag, const char *pwd)
{
    port_lock();
    snprintf(L->ufrag, sizeof(L->ufrag), "%s", ufrag);
    snprintf(L->pwd, sizeof(L->pwd), "%s", pwd);
    port_unlock();
}

void rtc_link_set_remote(rtc_link_t *L, const remote_offer_t *remote)
{
    port_lock();
    L->pending = *remote;
    L->remote_new = true;
    port_unlock();
}

bool rtc_link_global_init(void)
{
    const rtc_identity_t *id = rtc_identity();
    mbedtls_x509_crt_init(&G.own_cert);
    mbedtls_pk_init(&G.own_key);
    mbedtls_entropy_init(&G.entropy);
    mbedtls_ctr_drbg_init(&G.drbg);
    int rc = mbedtls_ctr_drbg_seed(&G.drbg, mbedtls_entropy_func, &G.entropy, (const unsigned char *)"dtls", 4);
    if (rc == 0) rc = mbedtls_x509_crt_parse_der(&G.own_cert, id->cert_der, id->cert_der_len);
    if (rc == 0) {
        rc = mbedtls_pk_parse_key(&G.own_key, id->key_der, id->key_der_len, NULL, 0, mbedtls_ctr_drbg_random, &G.drbg);
    }
    if (rc != 0) {
        SLOGE(TAG, "DTLS identity: -0x%04x", (unsigned)-rc);
        return false;
    }
    return true;
}

bool rtc_link_init(rtc_link_t *L, const char *name, uint16_t port, const rtc_link_ops_t *ops)
{
    memset(L, 0, sizeof(*L));
    L->name = name;
    L->ops = ops;
    L->port = port;
    L->ping_us = -1;
    port_random(L->tiebreaker, sizeof(L->tiebreaker));
    port_random(&L->ssrc, sizeof(L->ssrc));
    L->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (L->sock < 0 || bind(L->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SLOGE(TAG, "cannot bind UDP %u", (unsigned)port);
        return false;
    }
    SLOGI(TAG, "%s link on UDP %u", name, (unsigned)port);
    return true;
}
