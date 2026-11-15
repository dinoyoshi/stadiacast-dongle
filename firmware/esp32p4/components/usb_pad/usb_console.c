/*
 * The dongle's console: a USB serial port (CDC ACM) next to the gamepad and the headset, for the
 * web console (firmware/esp32p4/console/, Web Serial in Chrome or Edge) or any terminal.
 *
 * A small application class driver of our own, like the gamepad's and the headset's, rather than
 * TinyUSB's built-in CDC class (which esp_tinyusb would also want to configure). Two interfaces
 * behind an interface association: the communication interface (02/02/00, one interrupt IN for
 * notifications, which this driver never sends) and the data interface (0A) with bulk OUT and IN.
 * Protocol 00 (no AT commands) keeps modem managers from probing the port. Linux binds cdc_acm
 * (/dev/ttyACM*), Windows 10+ binds usbser (a COM port), macOS its CDC driver.
 *
 * Out (to the host): usb_console_write() appends whole lines to a ring under a spinlock, from any
 * task; the USB task or the writer sends what is there, up to one packet less a byte at a time
 * (a short packet ends every transfer, so no host waits for a zero-length one). Nothing is kept
 * while the port is closed: the host asserts DTR when it opens it, and any line it sends counts
 * as open for a while too, for hosts that do not.
 * In (from the host): bytes up to a newline become a line in a short queue that
 * usb_console_read_line() takes from.
 */
#include "usb_console.h"
#include "usb_pad.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "usb_console";

#define TX_RING 8192 /* a power of two */
#define RX_LINES 6
#define OPEN_BY_RX_US (5 * 1000 * 1000)

/* CDC class requests */
#define CDC_SET_LINE_CODING 0x20
#define CDC_GET_LINE_CODING 0x21
#define CDC_SET_CONTROL_LINE_STATE 0x22
#define CDC_SEND_BREAK 0x23

static struct {
    bool ready; /* usb_console_init() succeeded */
    uint8_t rhport, comm_itf, data_itf;
    volatile bool opened; /* the host configured the function */
    volatile bool dtr;
    volatile int64_t last_rx_us;
    uint16_t mps;
    uint8_t line_coding[7];
    /* buffers for the endpoints: DMA-capable, cache-line aligned, from the heap (not .bss, which
     * comes out of the small RAM pool v1.x P4 chips start from) */
    uint8_t *ep_in, *ep_out;
    /* out */
    portMUX_TYPE mux;
    char *ring;
    uint32_t head, tail; /* free-running, under mux */
    uint32_t dropped;
    /* in */
    char line[USB_CONSOLE_LINE_MAX];
    size_t line_len;
    QueueHandle_t rx;
} C = {.mux = portMUX_INITIALIZER_UNLOCKED};

bool usb_console_init(void)
{
    if (C.ready) return true;
    C.ring = heap_caps_malloc(TX_RING, MALLOC_CAP_8BIT);
    C.ep_in = heap_caps_aligned_calloc(64, 1, USB_CONSOLE_EP_SIZE_HS, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    C.ep_out = heap_caps_aligned_calloc(64, 1, USB_CONSOLE_EP_SIZE_HS, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    C.rx = xQueueCreate(RX_LINES, USB_CONSOLE_LINE_MAX);
    const uint8_t lc[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8}; /* 115200 8N1: the host's to choose */
    memcpy(C.line_coding, lc, sizeof(lc));
    C.ready = C.ring && C.ep_in && C.ep_out && C.rx;
    if (!C.ready) ESP_LOGE(TAG, "no memory for the USB console");
    return C.ready;
}

bool usb_console_active(void)
{
    if (!C.opened) return false;
    return C.dtr || esp_timer_get_time() - C.last_rx_us < OPEN_BY_RX_US;
}

/* ---- out ---------------------------------------------------------------------------- */

static void try_send(void)
{
    if (!C.opened || !tud_ready()) return;
    uint8_t ep = USB_CONSOLE_EP_DATA_IN;
    if (!usbd_edpt_claim(C.rhport, ep)) return;
    size_t n = 0, max = (size_t)C.mps - 1;
    portENTER_CRITICAL(&C.mux);
    uint32_t avail = C.head - C.tail;
    if (avail > max) avail = max;
    for (; n < avail; n++) C.ep_in[n] = (uint8_t)C.ring[(C.tail + n) & (TX_RING - 1)];
    C.tail += (uint32_t)n;
    portEXIT_CRITICAL(&C.mux);
    if (!n || !usbd_edpt_xfer(C.rhport, ep, C.ep_in, (uint16_t)n, false)) usbd_edpt_release(C.rhport, ep);
}

bool usb_console_write(const char *s, size_t len)
{
    if (!C.ready || !usb_console_active()) return false;
    bool ok;
    portENTER_CRITICAL(&C.mux);
    ok = len <= TX_RING - (C.head - C.tail);
    if (ok) {
        for (size_t i = 0; i < len; i++) C.ring[(C.head + i) & (TX_RING - 1)] = s[i];
        C.head += (uint32_t)len;
    } else {
        C.dropped++;
    }
    portEXIT_CRITICAL(&C.mux);
    if (ok) try_send();
    return ok;
}

uint32_t usb_console_dropped(void)
{
    return C.dropped;
}

/* ---- in ----------------------------------------------------------------------------- */

bool usb_console_read_line(char *out, uint32_t timeout_ms)
{
    if (!C.ready) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    return xQueueReceive(C.rx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void on_rx(const uint8_t *data, size_t n)
{
    C.last_rx_us = esp_timer_get_time();
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (C.line_len) {
                C.line[C.line_len] = 0;
                if (xQueueSend(C.rx, C.line, 0) != pdTRUE) ESP_LOGW(TAG, "a command was dropped: too many at once");
            }
            C.line_len = 0;
        } else if (C.line_len < USB_CONSOLE_LINE_MAX - 1) {
            C.line[C.line_len++] = c;
        }
    }
}

/* ---- the class driver --------------------------------------------------------------- */

static void cdc_init(void)
{
}

static bool cdc_deinit(void)
{
    return true;
}

static void cdc_reset(uint8_t rhport)
{
    C.opened = false;
    C.dtr = false;
    C.line_len = 0;
}

/* The communication interface, its functional descriptors and endpoint, then the data interface
 * and its two endpoints. */
static uint16_t cdc_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (!C.ready || itf->bInterfaceClass != TUSB_CLASS_CDC || itf->bInterfaceSubClass != 0x02) return 0;
    C.rhport = rhport;
    C.comm_itf = itf->bInterfaceNumber;
    uint16_t len = 0;
    uint8_t const *p = (uint8_t const *)itf;
    bool data_seen = false;
    while (len < max_len) {
        uint8_t type = tu_desc_type(p);
        if (len && type == TUSB_DESC_INTERFACE_ASSOCIATION) break;
        if (type == TUSB_DESC_INTERFACE) {
            tusb_desc_interface_t const *d = (tusb_desc_interface_t const *)p;
            if (len && d->bInterfaceClass != TUSB_CLASS_CDC_DATA) break;
            if (d->bInterfaceClass == TUSB_CLASS_CDC_DATA) {
                if (data_seen) break;
                data_seen = true;
                C.data_itf = d->bInterfaceNumber;
            }
        } else if (type == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
            if (ep->bEndpointAddress == USB_CONSOLE_EP_DATA_IN) C.mps = tu_edpt_packet_size(ep);
        }
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    TU_VERIFY(data_seen && C.mps, 0);
    C.opened = true;
    C.dtr = false;
    usbd_edpt_xfer(rhport, USB_CONSOLE_EP_DATA_OUT, C.ep_out, C.mps, false);
    return len;
}

static bool cdc_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE || req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS ||
        tu_u16_low(req->wIndex) != C.comm_itf) {
        return false;
    }
    switch (req->bRequest) {
    case CDC_SET_LINE_CODING:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, req, C.line_coding, sizeof(C.line_coding));
        return true;
    case CDC_GET_LINE_CODING:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, req, C.line_coding, sizeof(C.line_coding));
        return true;
    case CDC_SET_CONTROL_LINE_STATE:
        if (stage == CONTROL_STAGE_SETUP) {
            bool dtr = req->wValue & 1;
            if (dtr != C.dtr) ESP_LOGI(TAG, "console %s by the host", dtr ? "opened" : "closed");
            C.dtr = dtr;
            if (!dtr) {
                portENTER_CRITICAL(&C.mux);
                C.tail = C.head; /* nobody reads it: drop what was waiting */
                portEXIT_CRITICAL(&C.mux);
            }
            return tud_control_status(rhport, req);
        }
        return true;
    case CDC_SEND_BREAK:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_status(rhport, req);
        return true;
    default: return false;
    }
}

static bool cdc_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n)
{
    if (ep_addr == USB_CONSOLE_EP_DATA_OUT) {
        if (result == XFER_RESULT_SUCCESS && n) on_rx(C.ep_out, n);
        usbd_edpt_xfer(rhport, USB_CONSOLE_EP_DATA_OUT, C.ep_out, C.mps, false);
    } else if (ep_addr == USB_CONSOLE_EP_DATA_IN) {
        try_send();
    }
    return true;
}

const usbd_class_driver_t usb_console_driver = {
    .name = "CDC",
    .init = cdc_init,
    .deinit = cdc_deinit,
    .reset = cdc_reset,
    .open = cdc_open,
    .control_xfer_cb = cdc_control_xfer_cb,
    .xfer_cb = cdc_xfer_cb,
    .sof = NULL,
};
