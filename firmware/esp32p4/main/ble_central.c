/*
 * The ESP32-P4's Bluetooth LE central for pairing (dongle_ble_t, core/dongle/pair.c): the C6's
 * radio through ESP-Hosted's VHCI over SDIO (the C6 firmware has Bluetooth on by default) and
 * the NimBLE host on the P4. Each blocking call starts one NimBLE operation and waits for its
 * callback; NimBLE's own task reports scans, the link going down and notifications to the core.
 */
#include "dongle.h"

#include <string.h>

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
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "pair";

static struct {
    const dongle_ble_events_t *ev;
    SemaphoreHandle_t op;     /* one NimBLE operation finished (given by its callback) */
    volatile bool synced;     /* the host and the C6's controller talk */
    bool started;             /* Bluetooth is up */
    uint8_t own_addr_type;
    volatile int op_rc;
    volatile uint16_t mtu;
    volatile uint16_t conn;   /* BLE_HS_CONN_HANDLE_NONE without one */
    /* find_chrs */
    const uint8_t (*want)[16];
    uint16_t *found;
    int nwant;
    /* read_long */
    uint8_t *rbuf;
    size_t *rlen, rcap;
} B = {.conn = BLE_HS_CONN_HANDLE_NONE};

/* Wait for the operation's callback; its status (0 = done) or BLE_HS_ETIMEOUT. */
static int op_wait(int ms)
{
    if (xSemaphoreTake(B.op, pdMS_TO_TICKS(ms)) != pdTRUE) return BLE_HS_ETIMEOUT;
    return B.op_rc;
}

/* Before starting an operation: forget a late result of an earlier one. */
static void op_begin(void)
{
    xSemaphoreTake(B.op, 0);
}

static void op_done(int rc)
{
    B.op_rc = rc;
    xSemaphoreGive(B.op);
}

static void to_addr(dongle_ble_addr_t *out, const ble_addr_t *a)
{
    out->type = a->type;
    memcpy(out->val, a->val, sizeof(out->val));
}

/* ---- GAP: scan and connection -------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0) return 0;
        uint16_t uuid16 = 0;
        if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 2) {
            uuid16 = (uint16_t)(f.svc_data_uuid16[0] | f.svc_data_uuid16[1] << 8);
        }
        dongle_ble_addr_t a;
        to_addr(&a, &ev->disc.addr);
        B.ev->adv(&a, ev->disc.rssi, uuid16, (const char *)f.name, f.name ? f.name_len : 0);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        B.ev->scan_done();
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) B.conn = ev->connect.conn_handle;
        op_done(ev->connect.status);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        B.conn = BLE_HS_CONN_HANDLE_NONE;
        B.ev->disconnected(ev->disconnect.reason);
        op_done(BLE_HS_ENOTCONN); /* whatever was waiting: the link is gone */
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.om) {
            uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
            uint8_t b[64];
            if (n <= sizeof(b) && ble_hs_mbuf_to_flat(ev->notify_rx.om, b, sizeof(b), NULL) == 0) {
                B.ev->notify(ev->notify_rx.attr_handle, b, n);
            }
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        B.mtu = ev->mtu.value;
        return 0;
    default: return 0;
    }
}

static int central_scan_start(int ms)
{
    struct ble_gap_disc_params dp = {.passive = 0, .filter_duplicates = 0};
    return ble_gap_disc(B.own_addr_type, ms, &dp, gap_event, NULL);
}

static void central_scan_stop(void)
{
    ble_gap_disc_cancel();
}

static int central_connect(const dongle_ble_addr_t *addr, int timeout_ms)
{
    ble_addr_t a = {.type = addr->type};
    memcpy(a.val, addr->val, sizeof(a.val));
    B.conn = BLE_HS_CONN_HANDLE_NONE;
    op_begin();
    if (ble_gap_connect(B.own_addr_type, &a, timeout_ms, NULL, gap_event, NULL) != 0) return DONGLE_BLE_CONNECT_FAILED;
    int rc = op_wait(timeout_ms + 2000);
    if (rc != 0 || B.conn == BLE_HS_CONN_HANDLE_NONE) {
        if (B.conn == BLE_HS_CONN_HANDLE_NONE) ble_gap_conn_cancel();
        return rc ? rc : BLE_HS_ENOTCONN;
    }
    return 0;
}

static void central_disconnect(void)
{
    if (B.conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(B.conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 20 && B.conn != BLE_HS_CONN_HANDLE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- GATT ------------------------------------------------------------------------------ */

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status == 0) B.mtu = mtu;
    op_done(err->status);
    return 0;
}

static void central_exchange_mtu(int timeout_ms)
{
    B.mtu = 23;
    op_begin();
    if (ble_gattc_exchange_mtu(B.conn, on_mtu, NULL) == 0) op_wait(timeout_ms);
}

static unsigned central_mtu(void)
{
    return B.mtu;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        for (int i = 0; i < B.nwant; i++) {
            if (chr->uuid.u.type == BLE_UUID_TYPE_128 && memcmp(chr->uuid.u128.value, B.want[i], 16) == 0) {
                B.found[i] = chr->val_handle;
            }
        }
        return 0;
    }
    op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static int central_find_chrs(const uint8_t (*uuids)[16], int n, uint16_t *handles, int timeout_ms)
{
    for (int i = 0; i < n; i++) handles[i] = 0;
    B.want = uuids;
    B.found = handles;
    B.nwant = n;
    op_begin();
    int rc = ble_gattc_disc_all_chrs(B.conn, 1, 0xFFFF, on_chr, NULL);
    return rc ? rc : op_wait(timeout_ms);
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    op_done(err->status);
    return 0;
}

static int central_write(uint16_t handle, const void *data, size_t len, int timeout_ms)
{
    int rc;
    op_begin();
    if (len + 3 <= B.mtu) {
        rc = ble_gattc_write_flat(B.conn, handle, data, (uint16_t)len, on_write, NULL);
    } else {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
        if (!om) return BLE_HS_ENOMEM;
        rc = ble_gattc_write_long(B.conn, handle, 0, om, on_write, NULL);
    }
    return rc ? rc : op_wait(timeout_ms);
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0 && attr && attr->om) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (*B.rlen + n <= B.rcap && os_mbuf_copydata(attr->om, 0, n, B.rbuf + *B.rlen) == 0) *B.rlen += n;
        return 0;
    }
    op_done(err->status == BLE_HS_EDONE ? 0 : err->status);
    return 0;
}

static int central_read_long(uint16_t handle, uint8_t *buf, size_t cap, size_t *len, int timeout_ms)
{
    B.rbuf = buf;
    B.rcap = cap;
    B.rlen = len;
    op_begin();
    int rc = ble_gattc_read_long(B.conn, handle, 0, on_read, NULL);
    return rc ? rc : op_wait(timeout_ms);
}

static void central_cancel(void)
{
    if (B.synced) {
        ble_gap_disc_cancel();
        if (B.conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(B.conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (B.op) op_done(BLE_HS_EAPP);
}

/* ---- Bluetooth --------------------------------------------------------------------------- */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0 && ble_hs_id_infer_auto(0, &B.own_addr_type) == 0) {
        B.synced = true;
        ESP_LOGI(TAG, "Bluetooth ready (the C6's radio)");
    }
}

static void on_reset(int reason)
{
    B.synced = false;
    ESP_LOGW(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static const char *central_start(const dongle_ble_events_t *events)
{
    B.ev = events;
    if (B.started) return NULL;
    if (!B.op && !(B.op = xSemaphoreCreateBinary())) return "no memory";
    if (esp_hosted_bt_controller_init() != ESP_OK) return "the C6 has no Bluetooth (its firmware lacks it)";
    if (esp_hosted_bt_controller_enable() != ESP_OK) return "the C6's Bluetooth did not start";
    if (nimble_port_init() != ESP_OK) return "the Bluetooth host did not start";
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    B.started = true;
    for (int i = 0; i < 100 && !B.synced; i++) vTaskDelay(pdMS_TO_TICKS(50));
    return B.synced ? NULL : "Bluetooth did not come up";
}

const dongle_ble_t esp32p4_ble = {
    .start = central_start,
    .scan_start = central_scan_start,
    .scan_stop = central_scan_stop,
    .connect = central_connect,
    .exchange_mtu = central_exchange_mtu,
    .mtu = central_mtu,
    .find_chrs = central_find_chrs,
    .write = central_write,
    .read_long = central_read_long,
    .disconnect = central_disconnect,
    .cancel = central_cancel,
};
