/*
 * Pairing a Stadia controller with the dongle, without a computer: the Wi-Fi setup the Stadia
 * app used to do over Bluetooth LE (src/stadiacast/ble_provisioner.py, docs/protocol.md section
 * 11), done by the board itself through its Bluetooth LE central (dongle_ble_t; on the ESP32-P4
 * the C6's radio and NimBLE, esp32p4/main/ble_central.c).
 *
 * A controller in setup mode (its light pulses orange: never set up, or turned off and then
 * started with Y held while pressing the Stadia button) advertises Fast Pair service data
 * (0xFE2C). The dongle
 *   1. scans for it (up to PAIR_SCAN_MS) and connects to the likeliest device it hears (named
 *      Stadia, then the strongest signal); phones and earbuds advertise Fast Pair too, so a device
 *      without the setup service is skipped and the next one tried,
 *   2. reads the controller's RSA public key from the crypto window (pair 1a: page index to the
 *      control characteristic, then a long read of the data characteristic, 512 bytes a page,
 *      gzip-compressed JSON {"device_info": {"public_key": <base64 SubjectPublicKeyInfo>}}),
 *   3. writes {"ssid", "wpa_auth": 7, "wpa_cipher": 4, "enc_passwd"} to the connect
 *      characteristic: the hotspot's own name, and its password under the controller's key
 *      (RSA PKCS#1 v1.5, base64),
 *   4. watches the setup state: 0x3c (joined) or the controller dropping the link (it leaves setup
 *      mode) is success; 0x4f, 0x34 or 0x4e failure.
 * After that the controller joins the hotspot. Its first boot after setup registers but never
 * connects (protocol.md 11): give it a minute, turn it off (hold the Stadia button) and wake it.
 *
 * Bluetooth starts on the first pairing and stays up (idle) until the next restart. The flow runs
 * in its own task; the board's Bluetooth callbacks only hand results over.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dongle_core.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "stadia_port.h"

static const char *TAG = "pair";

#define PAIR_SCAN_MS 60000   /* for finding a controller, all tries together */
#define PAIR_CONNECT_MS 10000
#define PAIR_OP_MS 10000     /* one GATT operation */
#define PAIR_WAIT_MS 45000   /* for the controller's verdict after the credentials */
#define WINDOW_PAGE 512
#define WINDOW_PAGES 24
#define KEY_JSON_MAX (64 * 1024)
#define FAST_PAIR_UUID16 0xFE2C

/* The setup service's characteristics (ble_provisioner.py) */
#define UUID_CONNECT "0328fe40-002f-11e5-87d0-0002a5d5c51b"
#define UUID_STATE "0c7c6160-fab2-11e4-9fbb-0002a5d5c51b"
#define UUID_CRYPTO_DATA "1a7c6160-fab2-11e4-9fbb-0002a5d5c51b"
#define UUID_CRYPTO_CTRL "1a6c6160-fab2-11e4-9fbb-0002a5d5c51b"

enum { ST_CONNECTING = 0x1E, ST_JSON_BAD = 0x34, ST_CONNECTED = 0x3C, ST_CMD_BAD = 0x4E, ST_JOIN_FAILED = 0x4F };

typedef enum { CH_CONNECT, CH_STATE, CH_CRYPTO_DATA, CH_CRYPTO_CTRL, CH_COUNT } chr_t;
static const char *const CH_UUIDS[CH_COUNT] = {UUID_CONNECT, UUID_STATE, UUID_CRYPTO_DATA, UUID_CRYPTO_CTRL};

static struct {
    const dongle_ble_t *ble;
    const char *ssid, *password;
    port_mutex_t *lock;       /* the status */
    port_event_t *ev;         /* the verdict, the link going down, or a stop */
    volatile bool running;    /* the pairing task is alive */
    volatile bool cancel;
    /* scan */
    volatile bool found;
    dongle_ble_addr_t addr;
    int rssi;
    char name[32];
    /* the connection */
    volatile bool dropped;
    volatile int drop_reason;
    uint16_t handle[CH_COUNT];
    uint8_t uuid[CH_COUNT][16];
    /* the crypto window */
    uint8_t *rbuf;
    size_t rlen, rcap;
    /* the setup state the controller notifies */
    volatile int setup_state; /* -1: none yet */
    volatile int verdict;     /* the first of 0x3c / failure states after the credentials, -1 none */
    volatile bool watching;
    /* for the console */
    const char *state;
    char msg[160];
    volatile uint32_t gen; /* bumped on every change */
} P = {.state = "idle", .setup_state = -1, .verdict = -1};

/* What a scan heard, and what the flow already tried this pairing. */
#define MAX_CAND 12
#define MAX_TRIES 2      /* connection attempts per device */
#define SCAN_SETTLE_MS 3000 /* keep listening this long after the first match */
typedef struct {
    dongle_ble_addr_t addr;
    int rssi;
    bool named; /* its name says Stadia */
    char name[32];
} cand_t;
static struct {
    cand_t c[MAX_CAND];
    volatile int n;
    volatile bool done; /* the scan ended by itself */
    struct {
        dongle_ble_addr_t addr;
        uint8_t tries;
        bool not_controller; /* connected, no setup service */
    } t[MAX_CAND];
    int nt;
} S;

/* ---- status ------------------------------------------------------------------------- */

static void set_status(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(const char *state, const char *fmt, ...)
{
    port_mutex_lock(P.lock);
    P.state = state;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P.msg, sizeof(P.msg), fmt, ap);
    va_end(ap);
    P.gen++;
    port_mutex_unlock(P.lock);
    SLOGI(TAG, "%s: %s", state, P.msg);
}

void pair_get_status(pair_status_t *out)
{
    if (!P.lock) {
        *out = (pair_status_t){.state = "idle", .setup_state = -1};
        return;
    }
    port_mutex_lock(P.lock);
    out->state = P.state;
    snprintf(out->msg, sizeof(out->msg), "%s", P.msg);
    snprintf(out->name, sizeof(out->name), "%s", P.found ? P.name : "");
    out->rssi = P.found ? P.rssi : 0;
    out->setup_state = P.setup_state;
    out->busy = P.running;
    out->gen = P.gen;
    port_mutex_unlock(P.lock);
}

/* ---- helpers ------------------------------------------------------------------------ */

/* "0328fe40-002f-11e5-87d0-0002a5d5c51b" into a little-endian 128-bit UUID, as on the air */
static void uuid_parse(const char *s, uint8_t u[16])
{
    uint8_t be[16];
    int n = 0;
    for (const char *p = s; *p && n < 16; p++) {
        if (*p == '-') continue;
        unsigned v;
        sscanf(p, "%2x", &v);
        be[n++] = (uint8_t)v;
        p++;
    }
    for (int i = 0; i < 16; i++) u[i] = be[15 - i];
}

static bool addr_eq(const dongle_ble_addr_t *a, const dongle_ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

/* ---- the board's Bluetooth events -------------------------------------------------------- */

static bool is_setup_mode(uint16_t svc_uuid16, const char *name, size_t name_len)
{
    if (svc_uuid16 == FAST_PAIR_UUID16) return true;
    if (name && name_len) {
        char n[32];
        size_t len = name_len < sizeof(n) - 1 ? name_len : sizeof(n) - 1;
        for (size_t i = 0; i < len; i++) n[i] = (char)(name[i] | 0x20);
        n[len] = 0;
        return strstr(n, "stadia") != NULL;
    }
    return false;
}

static void on_adv(const dongle_ble_addr_t *addr, int rssi, uint16_t svc_uuid16, const char *name, size_t name_len)
{
    /* Phones, earbuds and other Google devices advertise Fast Pair (0xFE2C) too: every match
     * is a candidate, tried likeliest first (pick_candidate). */
    if (!is_setup_mode(svc_uuid16, name, name_len)) return;
    port_mutex_lock(P.lock);
    cand_t *c = NULL;
    for (int i = 0; i < S.n; i++) {
        if (addr_eq(&S.c[i].addr, addr)) c = &S.c[i];
    }
    if (!c && S.n < MAX_CAND) {
        c = &S.c[S.n++];
        memset(c, 0, sizeof(*c));
        c->addr = *addr;
        snprintf(c->name, sizeof(c->name), "%02x:%02x:%02x:%02x:%02x:%02x", c->addr.val[5], c->addr.val[4],
                 c->addr.val[3], c->addr.val[2], c->addr.val[1], c->addr.val[0]);
    }
    if (c) {
        c->rssi = rssi;
        if (name && name_len) {
            size_t len = name_len < sizeof(c->name) - 1 ? name_len : sizeof(c->name) - 1;
            memcpy(c->name, name, len);
            c->name[len] = 0;
            char low[sizeof(c->name)];
            for (size_t i = 0; i <= len; i++) low[i] = (char)(c->name[i] ? c->name[i] | 0x20 : 0);
            c->named = strstr(low, "stadia") != NULL;
        }
    }
    port_mutex_unlock(P.lock);
}

static void on_scan_done(void)
{
    S.done = true;
}

static void on_disconnected(int reason)
{
    P.drop_reason = reason;
    P.dropped = true;
    port_event_set(P.ev); /* whatever was waiting: the link is gone */
}

static void on_notify(uint16_t handle, const uint8_t *data, size_t len)
{
    if (handle != P.handle[CH_STATE] || len == 0 || len > 8) return;
    int st = data[len - 1];
    P.setup_state = st;
    P.gen++;
    SLOGI(TAG, "controller setup state 0x%02x", st);
    if (P.watching && P.verdict < 0 &&
        (st == ST_CONNECTED || st == ST_JOIN_FAILED || st == ST_JSON_BAD || st == ST_CMD_BAD)) {
        P.verdict = st;
        port_event_set(P.ev);
    }
}

static const dongle_ble_events_t EVENTS = {on_adv, on_scan_done, on_disconnected, on_notify};

/* One window page (ble_provisioner.window_read): the index to the control characteristic, then
 * a long read of the data characteristic, appended to P.rbuf. Its length, or -1. */
static int window_page(int page)
{
    uint8_t idx[2] = {(uint8_t)(page & 0xFF), (uint8_t)(page >> 8)};
    if (P.ble->write(P.handle[CH_CRYPTO_CTRL], idx, sizeof(idx), PAIR_OP_MS) != 0) return -1;
    port_sleep_ms(400);
    size_t before = P.rlen;
    if (P.ble->read_long(P.handle[CH_CRYPTO_DATA], P.rbuf, P.rcap, &P.rlen, PAIR_OP_MS) != 0) return -1;
    return (int)(P.rlen - before);
}

/* ---- the key and the credentials --------------------------------------------------------- */

/* gzip (or plain) -> the JSON text, NUL-terminated, malloc'd; NULL on failure. */
static char *ungzip(const uint8_t *d, size_t n)
{
    char *out = port_alloc_bulk(KEY_JSON_MAX + 1);
    if (!out) return NULL;
    if (n >= 18 && d[0] == 0x1F && d[1] == 0x8B && d[2] == 8) {
        size_t r = port_gunzip(d, n, (uint8_t *)out, KEY_JSON_MAX);
        if (!r) {
            SLOGW(TAG, "the crypto window did not unpack");
            free(out);
            return NULL;
        }
        out[r] = 0;
    } else {
        size_t r = n < KEY_JSON_MAX ? n : KEY_JSON_MAX;
        memcpy(out, d, r);
        out[r] = 0;
    }
    return out;
}

/* A small JSON reader, enough for root.device_info.public_key: the whole text must be valid
 * JSON (trailing text is ignored), object keys match without regard to case, the first match
 * wins. */
typedef struct {
    const char *p;
} js_t;

static void js_ws(js_t *j)
{
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

static int js_hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = s[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        v = v << 4 | d;
    }
    return v;
}

/* A string at j->p (on '"'), its text into out (NUL-terminated, cut at cap; out may be NULL). */
static bool js_string(js_t *j, char *out, size_t cap)
{
    if (*j->p != '"') return false;
    const char *p = j->p + 1;
    size_t n = 0;
#define PUT(ch) do { if (out && n + 1 < cap) out[n] = (char)(ch); n++; } while (0)
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (!c) return false;
        if (c == '"') break;
        if (c != '\\') {
            PUT(c);
            p++;
            continue;
        }
        p++;
        switch (*p) {
        case '"': case '\\': case '/': PUT(*p); p++; break;
        case 'b': PUT('\b'); p++; break;
        case 'f': PUT('\f'); p++; break;
        case 'n': PUT('\n'); p++; break;
        case 'r': PUT('\r'); p++; break;
        case 't': PUT('\t'); p++; break;
        case 'u': {
            int u = js_hex4(p + 1);
            if (u < 0) return false;
            p += 5;
            long cp = u;
            if (u >= 0xD800 && u <= 0xDBFF) {
                int lo = p[0] == '\\' && p[1] == 'u' ? js_hex4(p + 2) : -1;
                if (lo < 0xDC00 || lo > 0xDFFF) return false;
                p += 6;
                cp = 0x10000 + ((long)(u - 0xD800) << 10) + (lo - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                return false;
            }
            if (cp < 0x80) {
                PUT(cp);
            } else if (cp < 0x800) {
                PUT(0xC0 | cp >> 6);
                PUT(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                PUT(0xE0 | cp >> 12);
                PUT(0x80 | (cp >> 6 & 0x3F));
                PUT(0x80 | (cp & 0x3F));
            } else {
                PUT(0xF0 | cp >> 18);
                PUT(0x80 | (cp >> 12 & 0x3F));
                PUT(0x80 | (cp >> 6 & 0x3F));
                PUT(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: return false;
        }
    }
#undef PUT
    if (out && cap) out[n < cap ? n : cap - 1] = 0;
    j->p = p + 1;
    return true;
}

static bool js_value(js_t *j, int depth);

static bool js_members(js_t *j, int depth, char close)
{
    j->p++;
    js_ws(j);
    if (*j->p == close) {
        j->p++;
        return true;
    }
    for (;;) {
        js_ws(j);
        if (close == '}') {
            if (!js_string(j, NULL, 0)) return false;
            js_ws(j);
            if (*j->p++ != ':') return false;
        }
        if (!js_value(j, depth + 1)) return false;
        js_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p++ != close) return false;
        return true;
    }
}

static bool js_value(js_t *j, int depth)
{
    if (depth > 1000) return false;
    js_ws(j);
    const char *p = j->p;
    if (*p == '{') return js_members(j, depth, '}');
    if (*p == '[') return js_members(j, depth, ']');
    if (*p == '"') return js_string(j, NULL, 0);
    if (!strncmp(p, "true", 4)) return j->p += 4, true;
    if (!strncmp(p, "false", 5)) return j->p += 5, true;
    if (!strncmp(p, "null", 4)) return j->p += 4, true;
    if (*p != '-' && (*p < '0' || *p > '9')) return false;
    char *end;
    strtod(p, &end);
    if (end == p) return false;
    j->p = end;
    return true;
}

/* In the object at j->p, the value of the first member named key (any case): j->p on it. */
static bool js_member(js_t *j, const char *key)
{
    js_ws(j);
    if (*j->p != '{') return false;
    j->p++;
    js_ws(j);
    if (*j->p == '}') return false;
    for (;;) {
        char k[32];
        js_ws(j);
        if (!js_string(j, k, sizeof(k))) return false; /* a longer key is cut, and matches none */
        bool match = strcasecmp(k, key) == 0;
        js_ws(j);
        if (*j->p++ != ':') return false;
        js_ws(j);
        if (match) return true;
        if (!js_value(j, 1)) return false;
        js_ws(j);
        if (*j->p++ != ',') return false;
    }
}

/* root.device_info.public_key, malloc'd; NULL when there is none or the JSON is broken. */
static char *json_public_key(const char *text)
{
    js_t j = {text};
    if (!js_value(&j, 0)) return NULL;
    j.p = text;
    if (!js_member(&j, "device_info") || !js_member(&j, "public_key") || *j.p != '"') return NULL;
    size_t cap = strlen(j.p);
    char *out = malloc(cap);
    if (out && !js_string(&j, out, cap)) {
        free(out);
        out = NULL;
    }
    return out;
}

/* The password under the controller's key: RSA PKCS#1 v1.5, base64 into out. */
static bool encrypt_password(const char *bundle_json, char *out, size_t cap)
{
    bool ok = false;
    char *key = json_public_key(bundle_json);
    if (!key) {
        SLOGW(TAG, "no device_info.public_key in the crypto window (%u bytes)", (unsigned)strlen(bundle_json));
        return false;
    }
    /* buffers and contexts on the heap: the entropy context alone is over 1 KB */
    struct {
        uint8_t der[1024], ct[512];
        mbedtls_pk_context pk;
        mbedtls_entropy_context ent;
        mbedtls_ctr_drbg_context drbg;
    } *w = calloc(1, sizeof(*w));
    if (!w) {
        free(key);
        return false;
    }
    uint8_t *der = w->der, *ct = w->ct;
    mbedtls_pk_context *pk = &w->pk;
    mbedtls_entropy_context *ent = &w->ent;
    mbedtls_ctr_drbg_context *drbg = &w->drbg;
    size_t der_len = 0, ct_len = 0, b64_len = 0;
    mbedtls_pk_init(pk);
    mbedtls_entropy_init(ent);
    mbedtls_ctr_drbg_init(drbg);
    const char *pw = P.password;
    int rc = mbedtls_base64_decode(der, sizeof(w->der), &der_len, (const uint8_t *)key, strlen(key));
    if (rc == 0) rc = mbedtls_pk_parse_public_key(pk, der, der_len);
    if (rc == 0 && mbedtls_pk_get_type(pk) != MBEDTLS_PK_RSA) rc = -1;
    if (rc == 0) rc = mbedtls_ctr_drbg_seed(drbg, mbedtls_entropy_func, ent, (const uint8_t *)"pair", 4);
    if (rc == 0) {
        rc = mbedtls_pk_encrypt(pk, (const uint8_t *)pw, strlen(pw), ct, &ct_len, sizeof(w->ct),
                                mbedtls_ctr_drbg_random, drbg);
    }
    if (rc == 0) rc = mbedtls_base64_encode((uint8_t *)out, cap, &b64_len, ct, ct_len);
    if (rc == 0) {
        SLOGI(TAG, "controller key: RSA %u bits", (unsigned)mbedtls_pk_get_bitlen(pk));
        ok = true;
    } else {
        SLOGW(TAG, "the controller's key could not be used: -0x%04x", (unsigned)-rc);
    }
    mbedtls_pk_free(pk);
    mbedtls_ctr_drbg_free(drbg);
    mbedtls_entropy_free(ent);
    free(w);
    free(key);
    return ok;
}

/* ---- the flow ---------------------------------------------------------------------------- */

/* This pairing's record of a device: how often it was tried, whether it is no controller. */
static int tried_index(const dongle_ble_addr_t *a, bool add)
{
    for (int i = 0; i < S.nt; i++) {
        if (addr_eq(&S.t[i].addr, a)) return i;
    }
    if (!add || S.nt >= MAX_CAND) return -1;
    S.t[S.nt].addr = *a;
    S.t[S.nt].tries = 0;
    S.t[S.nt].not_controller = false;
    return S.nt++;
}

static bool usable(const cand_t *c)
{
    int i = tried_index(&c->addr, false);
    return i < 0 || (!S.t[i].not_controller && S.t[i].tries < MAX_TRIES);
}

/* The likeliest candidate: named Stadia first, then the strongest signal; -1 for none. */
static int pick_candidate(void)
{
    int best = -1;
    for (int i = 0; i < S.n; i++) {
        if (!usable(&S.c[i])) continue;
        if (best < 0 || S.c[i].named > S.c[best].named ||
            (S.c[i].named == S.c[best].named && S.c[i].rssi > S.c[best].rssi)) {
            best = i;
        }
    }
    return best;
}

/* Scan until something usable has been heard (then a little longer, to hear the rest), the scan
 * ends, or `until`. The chosen candidate's index, or -1. */
static int scan_round(int64_t until)
{
    port_mutex_lock(P.lock);
    S.n = 0;
    S.done = false;
    port_mutex_unlock(P.lock);
    int64_t left = until - port_now_ms();
    if (left < 1000) return -1;
    if (P.ble->scan_start((int)left) != 0) return -1;
    int64_t settle = 0;
    while (!P.cancel && !S.done && port_now_ms() < until) {
        port_mutex_lock(P.lock);
        bool any = pick_candidate() >= 0;
        port_mutex_unlock(P.lock);
        if (any && !settle) settle = port_now_ms() + SCAN_SETTLE_MS;
        if (settle && port_now_ms() >= settle) break;
        port_sleep_ms(100);
    }
    if (!S.done) P.ble->scan_stop();
    port_sleep_ms(100); /* let the scan wind down */
    port_mutex_lock(P.lock);
    int i = pick_candidate();
    if (i >= 0) {
        for (int k = 0; k < S.n; k++) {
            SLOGI(TAG, "heard %s (%d dBm)%s", S.c[k].name, S.c[k].rssi, S.c[k].named ? ", named Stadia" : "");
        }
        P.addr = S.c[i].addr;
        P.rssi = S.c[i].rssi;
        snprintf(P.name, sizeof(P.name), "%s", S.c[i].name);
        P.found = true;
    }
    port_mutex_unlock(P.lock);
    return i;
}

/* Connect to P.addr and find the setup service. NULL when it is a controller in setup mode. */
static const char *connect_setup(bool *not_controller)
{
    *not_controller = false;
    set_status("connecting", "found %s (%d dBm), connecting", P.name, P.rssi);
    P.dropped = false;
    int rc = P.ble->connect(&P.addr, PAIR_CONNECT_MS);
    if (rc == DONGLE_BLE_CONNECT_FAILED) return "could not connect";
    if (rc != 0) return "the connection failed";
    P.ble->exchange_mtu(PAIR_OP_MS);
    memset(P.handle, 0, sizeof(P.handle));
    if (P.ble->find_chrs((const uint8_t (*)[16])P.uuid, CH_COUNT, P.handle, PAIR_OP_MS * 2) != 0) {
        return "its services could not be read";
    }
    for (int i = 0; i < CH_COUNT; i++) {
        if (!P.handle[i]) {
            SLOGW(TAG, "%s: characteristic %s missing: not a controller in setup mode", P.name, CH_UUIDS[i]);
            *not_controller = true;
            return "not a Stadia controller in setup mode";
        }
    }
    return NULL;
}

/* Returns NULL when the controller has the credentials, else why not. */
static const char *pair_run(void)
{
    /* 1 and 2. find a controller in setup mode and connect to it, trying the candidates in turn */
    static char why[160];
    int rc;
    P.found = false;
    S.nt = 0;
    int64_t until = port_now_ms() + PAIR_SCAN_MS;
    const char *last = NULL;
    for (;;) {
        if (P.cancel) return "cancelled";
        set_status("scanning", "looking for a controller in setup mode (light pulsing orange)");
        int i = scan_round(until);
        if (P.cancel) return "cancelled";
        if (i < 0) {
            if (!last) return "no controller in setup mode found within 60 s: is its light pulsing orange?";
            snprintf(why, sizeof(why), "no controller in setup mode answered within 60 s (last: %s): is its light "
                     "pulsing orange? Try again", last);
            return why;
        }
        int t = tried_index(&P.addr, true);
        if (t >= 0) S.t[t].tries++;
        bool not_controller = false;
        last = connect_setup(&not_controller);
        if (!last) break;
        if (t >= 0 && not_controller) S.t[t].not_controller = true;
        SLOGW(TAG, "%s: %s, trying the next device", P.name, last);
        P.ble->disconnect();
        P.found = false;
    }
    /* the state notifications: the CCCD right after the value */
    uint8_t on[2] = {1, 0};
    if (P.ble->write(P.handle[CH_STATE] + 1, on, sizeof(on), PAIR_OP_MS) != 0) SLOGW(TAG, "no state notifications");
    if (P.cancel) return "cancelled";

    /* 3. the controller's key */
    set_status("reading", "connected to %s (MTU %u), reading its key", P.name, P.ble->mtu());
    P.rcap = WINDOW_PAGES * WINDOW_PAGE;
    P.rlen = 0;
    P.rbuf = malloc(P.rcap);
    if (!P.rbuf) return "no memory";
    int page;
    for (page = 0; page < WINDOW_PAGES && !P.cancel; page++) {
        int n = window_page(page);
        if (n < 0) {
            free(P.rbuf);
            P.rbuf = NULL;
            return P.dropped ? "the controller dropped the link while its key was read: try again"
                             : "reading the controller's key failed: try again";
        }
        if (n < WINDOW_PAGE) break;
    }
    SLOGI(TAG, "crypto window: %u bytes in %d pages", (unsigned)P.rlen, page + 1);
    char *json = ungzip(P.rbuf, P.rlen);
    free(P.rbuf);
    P.rbuf = NULL;
    if (P.cancel) {
        free(json);
        return "cancelled";
    }
    static char enc[700];
    bool ok = json && encrypt_password(json, enc, sizeof(enc));
    free(json);
    if (!ok) return "the controller's key could not be read: try again";

    /* 4. the credentials: the hotspot, WPA2-Personal (AES) */
    char *payload = malloc(900);
    if (!payload) return "no memory";
    int len = snprintf(payload, 900, "{\"ssid\":\"%s\",\"wpa_auth\":7,\"wpa_cipher\":4,\"enc_passwd\":\"%s\"}",
                       P.ssid, enc);
    set_status("sending", "sending the hotspot \"%s\" to %s", P.ssid, P.name);
    P.verdict = -1;
    P.watching = true;
    rc = P.ble->write(P.handle[CH_CONNECT], payload, (size_t)len, PAIR_OP_MS);
    free(payload);
    if (rc != 0 && !P.dropped) {
        P.watching = false;
        return "the controller refused the credentials: try again";
    }

    /* 5. the verdict: a state, or the link going down as it leaves setup mode */
    set_status("waiting", "waiting for %s to join \"%s\"", P.name, P.ssid);
    until = port_now_ms() + PAIR_WAIT_MS;
    while (!P.cancel && !P.dropped && P.verdict < 0 && port_now_ms() < until) {
        port_event_wait(P.ev, 500);
    }
    P.watching = false;
    if (P.verdict == ST_CONNECTED || (P.dropped && P.verdict < 0)) return NULL;
    if (P.verdict == ST_JOIN_FAILED) return "the controller could not join the hotspot (state 0x4f)";
    if (P.verdict >= 0) return "the controller could not read the credentials: try again";
    if (P.cancel) return "cancelled";
    return "the controller gave no sign of joining within 45 s: check it is still in setup mode and try again";
}

static void pair_task(void *arg)
{
    const char *err = P.ble->start(&EVENTS);
    if (!err) {
        err = pair_run();
        bool joined = P.verdict == ST_CONNECTED;
        P.ble->disconnect();
        if (!err) {
            set_status("done",
                       "%s has the hotspot%s. Its first start after setup does not connect: wait a minute, turn it "
                       "off (hold the Stadia button), then wake it",
                       P.name, joined ? " and joined it" : " and left setup mode");
        }
    }
    if (err) set_status("failed", "%s", err);
    P.running = false;
}

/* ---- the console's side ------------------------------------------------------------------- */

void pair_setup(const dongle_ble_t *ble, const char *ssid, const char *password)
{
    P.ble = ble;
    P.ssid = ssid;
    P.password = password;
}

bool pair_init(void)
{
    if (P.lock) return true;
    if (!P.ble) return false;
    P.ev = port_event_new();
    for (int i = 0; i < CH_COUNT; i++) uuid_parse(CH_UUIDS[i], P.uuid[i]);
    P.lock = P.ev ? port_mutex_new() : NULL; /* last: pair_get_status reads it */
    return P.lock != NULL;
}

const char *pair_start(void)
{
    if (!pair_init()) return "no memory";
    if (P.running) return "a pairing is already running";
    P.cancel = false;
    P.found = false;
    P.setup_state = -1;
    P.verdict = -1;
    P.running = true;
    set_status("starting", "starting Bluetooth");
    /* the stack: mbedTLS RSA and the flow's buffers */
    if (!port_spawn_prio(pair_task, NULL, "pair", 12288, 3)) {
        P.running = false;
        set_status("failed", "no memory for the pairing task");
        return "no memory";
    }
    return NULL;
}

void pair_stop(void)
{
    if (!P.running) return;
    P.cancel = true;
    P.ble->cancel();
    port_event_set(P.ev);
}
