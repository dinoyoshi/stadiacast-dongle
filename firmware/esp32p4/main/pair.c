/*
 * Pairing a Stadia controller with the dongle, without a computer: the Wi-Fi setup the Stadia
 * app used to do over Bluetooth LE (src/stadiacast/ble_provisioner.py, docs/protocol.md section
 * 11), done by the board itself through the ESP32-C6's Bluetooth radio (ESP-Hosted's VHCI over
 * SDIO; the C6 firmware has Bluetooth on by default) and the NimBLE host on the P4.
 *
 * A controller in setup mode (its light pulses orange: never set up, or turned off and then
 * started with Y held while pressing the Stadia button) advertises Fast Pair service data
 * (0xFE2C). The dongle
 *   1. scans for it (up to PAIR_SCAN_MS) and connects to the first one it hears,
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
 * in its own task; NimBLE's callbacks only hand results over.
 */
#include "dongle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "miniz.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"

static const char *TAG = "pair";

#define PAIR_SCAN_MS 60000
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
    SemaphoreHandle_t lock;   /* the status */
    SemaphoreHandle_t op;     /* one NimBLE operation finished (given by its callback) */
    volatile bool synced;     /* the host and the C6's controller talk */
    volatile bool running;    /* the pairing task is alive */
    volatile bool cancel;
    bool started;             /* Bluetooth is up */
    uint8_t own_addr_type;
    /* the operation in flight */
    volatile int op_rc;
    volatile uint16_t mtu;
    /* scan */
    volatile bool found;
    ble_addr_t addr;
    int rssi;
    char name[32];
    /* the connection */
    volatile uint16_t conn;   /* BLE_HS_CONN_HANDLE_NONE without one */
    volatile bool dropped;
    volatile int drop_reason;
    uint16_t handle[CH_COUNT];
    ble_uuid128_t uuid[CH_COUNT];
    /* a long read */
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
} P = {.conn = BLE_HS_CONN_HANDLE_NONE, .state = "idle", .setup_state = -1, .verdict = -1};

/* ---- status ------------------------------------------------------------------------- */

static void set_status(const char *state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(const char *state, const char *fmt, ...)
{
    xSemaphoreTake(P.lock, portMAX_DELAY);
    P.state = state;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P.msg, sizeof(P.msg), fmt, ap);
    va_end(ap);
    P.gen++;
    xSemaphoreGive(P.lock);
    ESP_LOGI(TAG, "%s: %s", state, P.msg);
}

void pair_get_status(pair_status_t *out)
{
    if (!P.lock) {
        *out = (pair_status_t){.state = "idle", .setup_state = -1};
        return;
    }
    xSemaphoreTake(P.lock, portMAX_DELAY);
    out->state = P.state;
    snprintf(out->msg, sizeof(out->msg), "%s", P.msg);
    snprintf(out->name, sizeof(out->name), "%s", P.found ? P.name : "");
    out->rssi = P.found ? P.rssi : 0;
    out->setup_state = P.setup_state;
    out->busy = P.running;
    out->gen = P.gen;
    xSemaphoreGive(P.lock);
}

/* ---- helpers ------------------------------------------------------------------------ */

/* "0328fe40-002f-11e5-87d0-0002a5d5c51b" into NimBLE's little-endian 128-bit UUID */
static void uuid_parse(const char *s, ble_uuid128_t *u)
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
    u->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; i++) u->value[i] = be[15 - i];
}

/* Wait for the operation's callback; its status (0 = done) or BLE_HS_ETIMEOUT. */
static int op_wait(int ms)
{
    if (xSemaphoreTake(P.op, pdMS_TO_TICKS(ms)) != pdTRUE) return BLE_HS_ETIMEOUT;
    return P.op_rc;
}

/* Before starting an operation: forget a late result of an earlier one. */
static void op_begin(void)
{
    xSemaphoreTake(P.op, 0);
}

static void op_done(int rc)
{
    P.op_rc = rc;
    xSemaphoreGive(P.op);
}

/* ---- GAP: scan and connection -------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg);

static bool is_setup_mode(const struct ble_hs_adv_fields *f)
{
    if (f->svc_data_uuid16 && f->svc_data_uuid16_len >= 2) {
        uint16_t u = (uint16_t)(f->svc_data_uuid16[0] | f->svc_data_uuid16[1] << 8);
        if (u == FAST_PAIR_UUID16) return true;
    }
    if (f->name && f->name_len) {
        char n[32];
        size_t len = f->name_len < sizeof(n) - 1 ? f->name_len : sizeof(n) - 1;
        for (size_t i = 0; i < len; i++) n[i] = (char)(f->name[i] | 0x20);
        n[len] = 0;
        return strstr(n, "stadia") != NULL;
    }
    return false;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        if (P.found) return 0;
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0 || !is_setup_mode(&f)) return 0;
        xSemaphoreTake(P.lock, portMAX_DELAY);
        P.addr = ev->disc.addr;
        P.rssi = ev->disc.rssi;
        size_t len = f.name && f.name_len ? (f.name_len < sizeof(P.name) - 1 ? f.name_len : sizeof(P.name) - 1) : 0;
        if (len) {
            memcpy(P.name, f.name, len);
            P.name[len] = 0;
        } else {
            snprintf(P.name, sizeof(P.name), "%02x:%02x:%02x:%02x:%02x:%02x", P.addr.val[5], P.addr.val[4],
                     P.addr.val[3], P.addr.val[2], P.addr.val[1], P.addr.val[0]);
        }
        P.found = true;
        xSemaphoreGive(P.lock);
        ble_gap_disc_cancel(); /* no DISC_COMPLETE after a cancel */
        op_done(0);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        op_done(BLE_HS_ETIMEOUT);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) P.conn = ev->connect.conn_handle;
        op_done(ev->connect.status);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        P.conn = BLE_HS_CONN_HANDLE_NONE;
        P.drop_reason = ev->disconnect.reason;
        P.dropped = true;
        op_done(BLE_HS_ENOTCONN); /* whatever was waiting: the link is gone */
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.attr_handle == P.handle[CH_STATE] && ev->notify_rx.om) {
            uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
            uint8_t b[8];
            if (n && ble_hs_mbuf_to_flat(ev->notify_rx.om, b, sizeof(b), NULL) == 0) {
                int st = b[(n < sizeof(b) ? n : sizeof(b)) - 1];
                P.setup_state = st;
                P.gen++;
                ESP_LOGI(TAG, "controller setup state 0x%02x", st);
                if (P.watching && P.verdict < 0 &&
                    (st == ST_CONNECTED || st == ST_JOIN_FAILED || st == ST_JSON_BAD || st == ST_CMD_BAD)) {
                    P.verdict = st;
                    op_done(0);
                }
            }
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        P.mtu = ev->mtu.value;
        return 0;
    default: return 0;
    }
}

/* ---- GATT ------------------------------------------------------------------------------ */

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) P.mtu = mtu;
    op_done(err->status);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        for (int i = 0; i < CH_COUNT; i++) {
            if (ble_uuid_cmp(&chr->uuid.u, &P.uuid[i].u) == 0) P.handle[i] = chr->val_handle;
        }
        return 0;
    }
    op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    op_done(err->status);
    return 0;
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0 && attr && attr->om) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (P.rlen + n <= P.rcap && os_mbuf_copydata(attr->om, 0, n, P.rbuf + P.rlen) == 0) P.rlen += n;
        return 0;
    }
    op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static int gatt_write(uint16_t handle, const void *data, size_t len)
{
    int rc;
    op_begin();
    if (len + 3 <= P.mtu) {
        rc = ble_gattc_write_flat(P.conn, handle, data, (uint16_t)len, on_write, NULL);
    } else {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
        if (!om) return BLE_HS_ENOMEM;
        rc = ble_gattc_write_long(P.conn, handle, 0, om, on_write, NULL);
    }
    return rc ? rc : op_wait(PAIR_OP_MS);
}

/* One window page (ble_provisioner.window_read): the index to the control characteristic, then
 * a long read of the data characteristic, appended to P.rbuf. Its length, or -1. */
static int window_page(int page)
{
    uint8_t idx[2] = {(uint8_t)(page & 0xFF), (uint8_t)(page >> 8)};
    if (gatt_write(P.handle[CH_CRYPTO_CTRL], idx, sizeof(idx)) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(400));
    size_t before = P.rlen;
    op_begin();
    if (ble_gattc_read_long(P.conn, P.handle[CH_CRYPTO_DATA], 0, on_read, NULL) != 0 || op_wait(PAIR_OP_MS) != 0) {
        return -1;
    }
    return (int)(P.rlen - before);
}

/* ---- the key and the credentials --------------------------------------------------------- */

/* gzip (or plain) -> the JSON text, NUL-terminated, malloc'd; NULL on failure. */
static char *ungzip(const uint8_t *d, size_t n)
{
    char *out = heap_caps_malloc(KEY_JSON_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(KEY_JSON_MAX + 1);
    if (!out) return NULL;
    if (n >= 18 && d[0] == 0x1F && d[1] == 0x8B && d[2] == 8) {
        uint8_t flg = d[3];
        size_t p = 10;
        if (flg & 4) p += 2 + (size_t)(d[p] | d[p + 1] << 8); /* FEXTRA */
        if (flg & 8) while (p < n && d[p++]) {}           /* FNAME */
        if (flg & 16) while (p < n && d[p++]) {}          /* FCOMMENT */
        if (flg & 2) p += 2;                              /* FHCRC */
        if (p >= n) {
            free(out);
            return NULL;
        }
        size_t r = tinfl_decompress_mem_to_mem(out, KEY_JSON_MAX, d + p, n - p, 0);
        if (r == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
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

/* The password under the controller's key: RSA PKCS#1 v1.5, base64 into out. */
static bool encrypt_password(const char *bundle_json, char *out, size_t cap)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(bundle_json);
    cJSON *info = root ? cJSON_GetObjectItem(root, "device_info") : NULL;
    cJSON *key = info ? cJSON_GetObjectItem(info, "public_key") : NULL;
    if (!cJSON_IsString(key)) {
        ESP_LOGW(TAG, "no device_info.public_key in the crypto window (%u bytes)", (unsigned)strlen(bundle_json));
        cJSON_Delete(root);
        return false;
    }
    uint8_t der[1024], ct[512];
    size_t der_len = 0, ct_len = 0, b64_len = 0;
    mbedtls_pk_context pk;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    const char *pw = CONFIG_DONGLE_PASSWORD;
    int rc = mbedtls_base64_decode(der, sizeof(der), &der_len, (const uint8_t *)key->valuestring, strlen(key->valuestring));
    if (rc == 0) rc = mbedtls_pk_parse_public_key(&pk, der, der_len);
    if (rc == 0 && mbedtls_pk_get_type(&pk) != MBEDTLS_PK_RSA) rc = -1;
    if (rc == 0) rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const uint8_t *)"pair", 4);
    if (rc == 0) {
        rc = mbedtls_pk_encrypt(&pk, (const uint8_t *)pw, strlen(pw), ct, &ct_len, sizeof(ct), mbedtls_ctr_drbg_random,
                                &drbg);
    }
    if (rc == 0) rc = mbedtls_base64_encode((uint8_t *)out, cap, &b64_len, ct, ct_len);
    if (rc == 0) {
        ESP_LOGI(TAG, "controller key: RSA %u bits", (unsigned)mbedtls_pk_get_bitlen(&pk));
        ok = true;
    } else {
        ESP_LOGW(TAG, "the controller's key could not be used: -0x%04x", (unsigned)-rc);
    }
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    cJSON_Delete(root);
    return ok;
}

/* ---- the flow ---------------------------------------------------------------------------- */

static void disconnect(void)
{
    if (P.conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(P.conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 20 && P.conn != BLE_HS_CONN_HANDLE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Returns NULL when the controller has the credentials, else why not. */
static const char *pair_run(void)
{
    /* 1. find a controller in setup mode */
    P.found = false;
    xSemaphoreTake(P.op, 0);
    set_status("scanning", "looking for a controller in setup mode (light pulsing orange)");
    struct ble_gap_disc_params dp = {.passive = 0, .filter_duplicates = 1};
    if (ble_gap_disc(P.own_addr_type, PAIR_SCAN_MS, &dp, gap_event, NULL) != 0) return "Bluetooth scan did not start";
    int rc = op_wait(PAIR_SCAN_MS + 2000);
    if (P.cancel) return "cancelled";
    if (!P.found) {
        ble_gap_disc_cancel();
        return "no controller in setup mode found within 60 s: is its light pulsing orange?";
    }

    /* 2. connect, MTU, characteristics */
    set_status("connecting", "found %s (%d dBm), connecting", P.name, P.rssi);
    P.dropped = false;
    P.conn = BLE_HS_CONN_HANDLE_NONE;
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the scan wind down */
    xSemaphoreTake(P.op, 0);
    if (ble_gap_connect(P.own_addr_type, &P.addr, 30000, NULL, gap_event, NULL) != 0) return "could not connect";
    rc = op_wait(32000);
    if (rc != 0 || P.conn == BLE_HS_CONN_HANDLE_NONE) return "the connection failed: try again";
    P.mtu = 23;
    op_begin();
    if (ble_gattc_exchange_mtu(P.conn, on_mtu, NULL) == 0) op_wait(PAIR_OP_MS);
    memset(P.handle, 0, sizeof(P.handle));
    op_begin();
    if (ble_gattc_disc_all_chrs(P.conn, 1, 0xFFFF, on_chr, NULL) != 0 || op_wait(PAIR_OP_MS * 2) != 0) {
        return "the controller's setup service could not be read";
    }
    for (int i = 0; i < CH_COUNT; i++) {
        if (!P.handle[i]) {
            ESP_LOGW(TAG, "characteristic %s missing", CH_UUIDS[i]);
            return "this is not a Stadia controller's setup service";
        }
    }
    /* the state notifications: the CCCD right after the value */
    uint8_t on[2] = {1, 0};
    if (gatt_write(P.handle[CH_STATE] + 1, on, sizeof(on)) != 0) ESP_LOGW(TAG, "no state notifications");
    if (P.cancel) return "cancelled";

    /* 3. the controller's key */
    set_status("reading", "connected to %s (MTU %u), reading its key", P.name, (unsigned)P.mtu);
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
    ESP_LOGI(TAG, "crypto window: %u bytes in %d pages", (unsigned)P.rlen, page + 1);
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
                       CONFIG_DONGLE_SSID, enc);
    set_status("sending", "sending the hotspot \"%s\" to %s", CONFIG_DONGLE_SSID, P.name);
    P.verdict = -1;
    P.watching = true;
    rc = gatt_write(P.handle[CH_CONNECT], payload, (size_t)len);
    free(payload);
    if (rc != 0 && !P.dropped) {
        P.watching = false;
        return "the controller refused the credentials: try again";
    }

    /* 5. the verdict: a state, or the link going down as it leaves setup mode */
    set_status("waiting", "waiting for %s to join \"%s\"", P.name, CONFIG_DONGLE_SSID);
    int64_t until = esp_log_timestamp() + PAIR_WAIT_MS;
    while (!P.cancel && !P.dropped && P.verdict < 0 && (int64_t)esp_log_timestamp() < until) {
        op_wait(500);
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
    const char *err = pair_run();
    bool joined = P.verdict == ST_CONNECTED;
    disconnect();
    if (err) {
        set_status("failed", "%s", err);
    } else {
        set_status("done",
                   "%s has the hotspot%s. Its first start after setup does not connect: wait a minute, turn it off "
                   "(hold the Stadia button), then wake it",
                   P.name, joined ? " and joined it" : " and left setup mode");
    }
    P.running = false;
    vTaskDelete(NULL);
}

/* ---- Bluetooth --------------------------------------------------------------------------- */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &P.own_addr_type) == 0) {
        P.synced = true;
        ESP_LOGI(TAG, "Bluetooth ready (the C6's radio)");
    }
}

static void on_reset(int reason)
{
    P.synced = false;
    ESP_LOGW(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static const char *bt_start(void)
{
    if (P.started) return NULL;
    if (esp_hosted_bt_controller_init() != ESP_OK) return "the C6 has no Bluetooth (its firmware lacks it)";
    if (esp_hosted_bt_controller_enable() != ESP_OK) return "the C6's Bluetooth did not start";
    if (nimble_port_init() != ESP_OK) return "the Bluetooth host did not start";
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    P.started = true;
    for (int i = 0; i < 100 && !P.synced; i++) vTaskDelay(pdMS_TO_TICKS(50));
    return P.synced ? NULL : "Bluetooth did not come up";
}

static void start_task(void *arg)
{
    const char *err = bt_start();
    if (err) {
        set_status("failed", "%s", err);
        P.running = false;
        vTaskDelete(NULL);
        return;
    }
    pair_task(arg);
}

bool pair_init(void)
{
    if (P.lock) return true;
    P.lock = xSemaphoreCreateMutex();
    P.op = xSemaphoreCreateBinary();
    for (int i = 0; i < CH_COUNT; i++) uuid_parse(CH_UUIDS[i], &P.uuid[i]);
    return P.lock && P.op;
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
    if (xTaskCreate(start_task, "pair", 8192, NULL, 3, NULL) != pdPASS) {
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
    if (P.synced) {
        ble_gap_disc_cancel();
        if (P.conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(P.conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    op_done(BLE_HS_EAPP);
}
