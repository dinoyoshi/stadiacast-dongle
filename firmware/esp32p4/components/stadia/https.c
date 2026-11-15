/*
 * The HTTPS server the controller's Google names resolve to (port of http_server.py).
 *
 * Each connection gets its own task. Before TLS starts we read the ClientHello ourselves:
 * one for play.googleapis.com (Clearcut) is answered with a fatal handshake_failure alert,
 * which makes the firmware give up on the upload at once instead of powering off or waiting
 * 15 s (docs/protocol.md section 7). Every other one is handed to mbedTLS with the built-in
 * GTS credential.
 */
#include "stadia_internal.h"
#include "stadia_port.h"
#include "builtin_cred.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "https";

#define MAX_CONNECTIONS 4
#define HEAD_MAX 4096
#define BODY_MAX (128 * 1024)
#define SCRATCH_MAX (16 * 1024)
#define IDLE_TIMEOUT_S 75
#define TLS_RECORD_MAX (5 + 16384)
#define CLEARCUT_HOST "play.googleapis.com"

static const uint8_t HANDSHAKE_FAILURE_ALERT[] = {21, 3, 3, 0, 2, 2, 40};

static mbedtls_x509_crt s_chain;
static int s_active;

typedef struct {
    int fd;
    char peer[16];
    uint8_t *hello; /* the ClientHello we read before TLS, served to mbedTLS first */
    size_t hello_len, hello_off;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_pk_context key;
} conn_t;

/* ---- reading the ClientHello --------------------------------------------------------- */

static bool read_full(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

/* The server_name in a ClientHello record, or "" if there is none. */
static void client_hello_sni(const uint8_t *rec, size_t len, char *out, size_t out_len)
{
    out[0] = '\0';
    /* record header (5) + handshake header (4) + version (2) + random (32) */
    size_t p = 5 + 4 + 2 + 32;
    if (len < p + 1 || rec[0] != 22 || rec[5] != 1) {
        return;
    }
    p += 1 + rec[p]; /* session id */
    if (p + 2 > len) return;
    p += 2 + ((rec[p] << 8) | rec[p + 1]); /* cipher suites */
    if (p + 1 > len) return;
    p += 1 + rec[p]; /* compression methods */
    if (p + 2 > len) return;
    size_t ext_end = p + 2 + ((rec[p] << 8) | rec[p + 1]);
    p += 2;
    if (ext_end > len) ext_end = len;
    while (p + 4 <= ext_end) {
        unsigned type = (rec[p] << 8) | rec[p + 1];
        size_t elen = (rec[p + 2] << 8) | rec[p + 3];
        p += 4;
        if (p + elen > ext_end) return;
        if (type == 0 && elen >= 5 && rec[p + 2] == 0) { /* server_name, host_name entry */
            size_t nlen = (rec[p + 3] << 8) | rec[p + 4];
            if (5 + nlen <= elen) {
                size_t n = nlen < out_len - 1 ? nlen : out_len - 1;
                memcpy(out, rec + p + 5, n);
                out[n] = '\0';
            }
            return;
        }
        p += elen;
    }
}

/* ---- mbedTLS I/O -------------------------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    conn_t *c = ctx;
    ssize_t r = send(c->fd, buf, len, 0);
    if (r < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    conn_t *c = ctx;
    if (c->hello_off < c->hello_len) {
        size_t n = c->hello_len - c->hello_off;
        n = n < len ? n : len;
        memcpy(buf, c->hello + c->hello_off, n);
        c->hello_off += n;
        return (int)n;
    }
    ssize_t r = recv(c->fd, buf, len, 0);
    if (r == 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }
    if (r < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)r;
}

static bool tls_write_all(conn_t *c, const uint8_t *buf, size_t len)
{
    while (len) {
        int r = mbedtls_ssl_write(&c->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        buf += r;
        len -= (size_t)r;
    }
    return true;
}

/* ---- HTTP framing ------------------------------------------------------------------- */

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Error";
    }
}

static void header_value(const char *line, const char *name, char *out, size_t out_len)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') {
        return;
    }
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t len = strcspn(v, "\r\n");
    while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
    len = len < out_len - 1 ? len : out_len - 1;
    memcpy(out, v, len);
    out[len] = '\0';
}

/* Parse the request head in `head` (NUL-terminated, ending with the blank line). */
static bool parse_head(char *head, http_req_t *req, size_t *content_length)
{
    char target[400] = "", version[16] = "";
    if (sscanf(head, "%7s %399s %15s", req->method, target, version) != 3) {
        return false;
    }
    char *q = strchr(target, '?');
    if (q) {
        *q++ = '\0';
        snprintf(req->query, sizeof(req->query), "%.191s", q);
    }
    snprintf(req->path, sizeof(req->path), "%.191s", target);
    req->wants_close = strcmp(version, "HTTP/1.0") == 0;
    *content_length = 0;
    char conn[32] = "", clen[24] = "";
    for (char *line = strstr(head, "\r\n"); line && line[2] != '\r'; line = strstr(line + 2, "\r\n")) {
        const char *l = line + 2;
        header_value(l, "Host", req->host, sizeof(req->host));
        header_value(l, "Content-Length", clen, sizeof(clen));
        header_value(l, "Connection", conn, sizeof(conn));
        header_value(l, "controller-device-id", req->device_id, sizeof(req->device_id));
        header_value(l, "controller-cloud-device-id", req->cloud_device_id, sizeof(req->cloud_device_id));
    }
    if (strcasecmp(conn, "close") == 0) req->wants_close = true;
    if (strcasecmp(conn, "keep-alive") == 0) req->wants_close = false;
    char *colon = strchr(req->host, ':'); /* the firmware sends "Host: name:443" */
    if (colon) *colon = '\0';
    *content_length = clen[0] ? strtoul(clen, NULL, 10) : 0;
    return true;
}

static bool send_response(conn_t *c, const http_resp_t *resp)
{
    char date[40];
    time_t now = (time_t)port_wall_time();
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: " STADIA_SERVER_NAME "/" STADIA_VERSION "\r\n",
                     resp->status, reason(resp->status), date);
    if (resp->content_type) {
        n += snprintf(head + n, sizeof(head) - n, "Content-Type: %s\r\n", resp->content_type);
    }
    n += snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\nConnection: %s\r\n\r\n",
                  (unsigned)resp->body_len, resp->close ? "close" : "keep-alive");
    return tls_write_all(c, (const uint8_t *)head, (size_t)n) &&
           (resp->body_len == 0 || tls_write_all(c, resp->body, resp->body_len));
}

/* Read one request into req (body malloc'ed). 0 = got one, 1 = connection over, -1 = bad. */
static int read_request(conn_t *c, char *head, size_t *carry, http_req_t *req)
{
    size_t have = *carry;
    char *end;
    head[have] = '\0';
    while (!(end = strstr(head, "\r\n\r\n"))) {
        if (have >= HEAD_MAX - 1) {
            return -1;
        }
        int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)head + have, HEAD_MAX - 1 - have);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r <= 0) {
            return 1;
        }
        have += (size_t)r;
        head[have] = '\0';
    }
    size_t head_len = (size_t)(end - head) + 4;
    end[2] = '\0'; /* keep the last header's CRLF for the parser */
    size_t clen;
    memset(req, 0, sizeof(*req));
    snprintf(req->peer, sizeof(req->peer), "%s", c->peer);
    if (!parse_head(head, req, &clen)) {
        return -1;
    }
    size_t extra = have - head_len; /* body bytes that came with the head */
    size_t keep = clen < BODY_MAX ? clen : BODY_MAX;
    req->body = malloc(keep + 1);
    if (!req->body) {
        return -1;
    }
    size_t from_head = extra < clen ? extra : clen;
    memcpy(req->body, head + head_len, from_head < keep ? from_head : keep);
    size_t got = from_head;
    uint8_t sink[512];
    while (got < clen) {
        uint8_t *dst = got < keep ? req->body + got : sink;
        size_t want = got < keep ? keep - got : (clen - got < sizeof(sink) ? clen - got : sizeof(sink));
        int r = mbedtls_ssl_read(&c->ssl, dst, want);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (r <= 0) {
            free(req->body);
            req->body = NULL;
            return 1;
        }
        got += (size_t)r;
    }
    req->body_len = keep;
    /* pipelined bytes after the body stay for the next request */
    size_t used = head_len + from_head;
    *carry = have > used ? have - used : 0;
    memmove(head, head + used, *carry);
    return 0;
}

/* ---- one connection ----------------------------------------------------------------- */

static void conn_task(void *arg)
{
    conn_t *c = arg;
    char sni[96] = "";
    uint8_t *scratch = NULL;
    char *head = NULL;

    uint8_t hdr[5];
    if (!read_full(c->fd, hdr, sizeof(hdr))) {
        goto done;
    }
    size_t rec_len = 5 + ((hdr[3] << 8) | hdr[4]);
    if (hdr[0] != 22 || rec_len > TLS_RECORD_MAX) {
        SLOGW(TAG, "%s: not a TLS handshake (first byte %u)", c->peer, hdr[0]);
        goto done;
    }
    c->hello = malloc(rec_len);
    if (!c->hello) {
        goto done;
    }
    memcpy(c->hello, hdr, 5);
    if (!read_full(c->fd, c->hello + 5, rec_len - 5)) {
        goto done;
    }
    c->hello_len = rec_len;
    client_hello_sni(c->hello, rec_len, sni, sizeof(sni));
    if (!g_cfg.allow_clearcut && strcasecmp(sni, CLEARCUT_HOST) == 0) {
        send(c->fd, HANDSHAKE_FAILURE_ALERT, sizeof(HANDSHAKE_FAILURE_ALERT), 0);
        SLOGI(TAG, "%s: refused the Clearcut upload (TLS alert handshake_failure)", c->peer);
        goto done;
    }

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_pk_init(&c->key);
    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, (const unsigned char *)"https", 5);
    if (rc == 0) {
        rc = mbedtls_pk_parse_key(&c->key, (const unsigned char *)BUILTIN_KEY_PEM, sizeof(BUILTIN_KEY_PEM), NULL, 0,
                                  mbedtls_ctr_drbg_random, &c->drbg);
    }
    if (rc == 0) {
        rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (rc == 0) {
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        rc = mbedtls_ssl_conf_own_cert(&c->conf, &s_chain, &c->key);
    }
    if (rc == 0) {
        rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    }
    if (rc != 0) {
        SLOGE(TAG, "TLS setup failed: -0x%04x", -rc);
        goto tls_done;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char why[100];
            mbedtls_strerror(rc, why, sizeof(why));
            SLOGW(TAG, "%s: TLS handshake for \"%s\" failed: %s", c->peer, sni, why);
            goto tls_done;
        }
    }
    free(c->hello);
    c->hello = NULL;

    head = malloc(HEAD_MAX);
    scratch = malloc(SCRATCH_MAX);
    if (!head || !scratch) {
        goto tls_done;
    }
    size_t carry = 0;
    for (;;) {
        http_req_t req;
        int r = read_request(c, head, &carry, &req);
        if (r != 0) {
            if (r < 0) {
                http_resp_t bad = {.status = 400, .close = true};
                send_response(c, &bad);
            }
            break;
        }
        http_resp_t resp = {0};
        receiver_handle(&req, &resp, scratch, SCRATCH_MAX);
        free(req.body);
        resp.close = resp.close || req.wants_close;
        SLOGI(TAG, "%s %s%s -> %d %s (%u bytes)%s", req.method, req.host, req.path, resp.status,
              resp.handler ? resp.handler : "", (unsigned)resp.body_len, resp.close ? ", close" : "");
        if (!send_response(c, &resp) || resp.close) {
            break;
        }
    }
    mbedtls_ssl_close_notify(&c->ssl);
tls_done:
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    mbedtls_pk_free(&c->key);
done:
    free(head);
    free(scratch);
    free(c->hello);
    close(c->fd);
    free(c);
    port_lock();
    s_active--;
    port_unlock();
}

static void listen_task(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int fd = accept(ls, (struct sockaddr *)&from, &flen);
        if (fd < 0) {
            port_sleep_ms(100);
            continue;
        }
        port_lock();
        bool room = s_active < MAX_CONNECTIONS;
        if (room) s_active++;
        port_unlock();
        if (!room) {
            SLOGW(TAG, "too many connections, closing one from %s", inet_ntoa(from.sin_addr));
            close(fd);
            continue;
        }
        struct timeval tv = {.tv_sec = IDLE_TIMEOUT_S};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        conn_t *c = calloc(1, sizeof(*c));
        c->fd = fd;
        snprintf(c->peer, sizeof(c->peer), "%s", inet_ntoa(from.sin_addr));
        if (!port_spawn(conn_task, c, "https_conn", 16 * 1024)) {
            close(fd);
            free(c);
            port_lock();
            s_active--;
            port_unlock();
        }
    }
}

bool https_start(uint16_t port)
{
    mbedtls_x509_crt_init(&s_chain);
    int rc = mbedtls_x509_crt_parse(&s_chain, (const unsigned char *)BUILTIN_CHAIN_PEM, sizeof(BUILTIN_CHAIN_PEM));
    if (rc != 0) {
        SLOGE(TAG, "built-in certificate chain does not parse: -0x%04x", -rc);
        return false;
    }
    int n = 0;
    for (const mbedtls_x509_crt *crt = &s_chain; crt && crt->raw.len; crt = crt->next) n++;
    SLOGI(TAG, "built-in credential: a chain of %d certificates (Google Trust Services)", n);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (ls < 0 || bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ls, 4) < 0) {
        SLOGE(TAG, "cannot listen on TCP %u", port);
        return false;
    }
    SLOGI(TAG, "listening on TCP %u", port);
    return port_spawn(listen_task, (void *)(intptr_t)ls, "https", 4096);
}
