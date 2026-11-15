/*
 * The gamepad as a TinyUSB application class driver, in the mode pad_modes.c describes.
 *
 * Xbox 360: the host's XUSB / xpad drivers bind by VID:PID 045E:028E and the vendor interface
 * FF/5D/01; input goes out as the 20-byte report on interrupt IN 0x81, rumble comes back on
 * interrupt OUT 0x01.
 * DualShock 4, Switch Pro, HID gamepad: a HID interface (class 3) with interrupt IN 0x81 and
 * OUT 0x01; this driver answers the HID class requests itself (report descriptor, GET_REPORT
 * for the DualShock 4's feature reports, SET_REPORT as another way in for output reports).
 *
 * With audio the device is composite (class EF/02/01): the gamepad stays interface 0 and the
 * UAC2 headphones of usb_audio.c are interfaces 1 and 2, behind an interface association.
 * Changing the mode takes the device off the bus (TinyUSB uninstalled) and brings it back with
 * the new descriptors.
 */
#include "usb_pad.h"
#include "pad_modes.h"
#include "usb_audio.h"

#include <stdlib.h>
#include <string.h>

#include "class/hid/hid.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb_pad";

#define PAD_EP_IN 0x81
#define PAD_EP_OUT 0x01
#define PAD_EP_SIZE 64
#define CONFIG_MAX 256

/* ---- descriptors -------------------------------------------------------------------- */

static const char *s_strings[] = {
    (const char[]){0x09, 0x04}, /* English */
    "Stadiacast",
    "Stadiacast Dongle",
    NULL, /* serial: from the chip's MAC */
    "Controller headset",
};

static const uint8_t s_audio_fs[] = {USB_AUDIO_DESC(1, USB_AUDIO_EP_SIZE_FS)};
static const uint8_t s_audio_hs[] = {USB_AUDIO_DESC(1, USB_AUDIO_EP_SIZE_HS)};
_Static_assert(sizeof(s_audio_hs) == USB_AUDIO_DESC_LEN, "audio descriptor length");

/* ---- state -------------------------------------------------------------------------- */

typedef struct {
    TUD_EPBUF_DEF(in, PAD_EP_SIZE);
    TUD_EPBUF_DEF(out, PAD_EP_SIZE);
} pad_epbuf_t;

CFG_TUD_MEM_SECTION static pad_epbuf_t s_buf;

static struct {
    usb_pad_mode_t mode;
    bool audio_ok, audio; /* audio_ok: the headphones can work; audio: in this mode's descriptors */
    uint8_t rhport;
    volatile bool open;
    SemaphoreHandle_t lock;
    usb_pad_state_t state;
    bool dirty;
    uint8_t reply[PAD_REPORT_MAX];
    size_t reply_len;
    usb_pad_rumble_cb_t rumble_cb;
    void *rumble_arg;
    uint8_t large, small;
    tusb_desc_device_t device;
    tusb_desc_device_qualifier_t qualifier;
    uint8_t *config_fs, *config_hs; /* CONFIG_MAX each, on the heap */
    uint8_t hid_desc[9];
    uint8_t ctrl[PAD_REPORT_MAX];
} s;

static void build_descriptors(void)
{
    const pad_mode_info_t *m = pad_mode_info(s.mode);
    s.audio = s.audio_ok && m->audio_ok;
    uint8_t cls = s.audio ? TUSB_CLASS_MISC : (m->report_desc ? 0x00 : 0xFF);
    uint8_t sub = s.audio ? MISC_SUBCLASS_COMMON : (m->report_desc ? 0x00 : 0xFF);
    uint8_t proto = s.audio ? MISC_PROTOCOL_IAD : (m->report_desc ? 0x00 : 0xFF);
    s.device = (tusb_desc_device_t){
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = cls,
        .bDeviceSubClass = sub,
        .bDeviceProtocol = proto,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = m->vid,
        .idProduct = m->pid,
        .bcdDevice = m->bcd,
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
        .bNumConfigurations = 1,
    };
    s.qualifier = (tusb_desc_device_qualifier_t){
        .bLength = sizeof(tusb_desc_device_qualifier_t),
        .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
        .bcdUSB = 0x0200,
        .bDeviceClass = cls,
        .bDeviceSubClass = sub,
        .bDeviceProtocol = proto,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .bNumConfigurations = 1,
        .bReserved = 0,
    };
    const uint8_t hid[9] = {9, HID_DESC_TYPE_HID, 0x11, 0x01, 0, 1, HID_DESC_TYPE_REPORT,
                            (uint8_t)(m->report_desc_len & 0xFF), (uint8_t)(m->report_desc_len >> 8)};
    memcpy(s.hid_desc, hid, sizeof(hid));

    for (int hs = 0; hs < 2; hs++) {
        uint8_t *c = hs ? s.config_hs : s.config_fs;
        /* bInterval: full speed in frames (1 ms), high speed as 2^(n-1) microframes */
        uint8_t in_iv = hs ? m->interval_hs : m->interval_fs;
        size_t n = 9;
        if (!m->report_desc) {
            uint8_t out_iv = hs ? 7 : 8; /* rumble: 8 ms */
            const uint8_t x[] = {
                /* interface 0: vendor FF/5D/01 with two endpoints */
                9, TUSB_DESC_INTERFACE, 0, 0, 2, 0xFF, 0x5D, 0x01, 0,
                /* the XUSB "unknown" descriptor (type 0x21) every wired 360 controller carries */
                17, 0x21, 0x00, 0x01, 0x01, 0x25, PAD_EP_IN, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, PAD_EP_OUT, 0x08,
                0x00, 0x00,
                7, TUSB_DESC_ENDPOINT, PAD_EP_IN, TUSB_XFER_INTERRUPT, 32, 0, in_iv,
                7, TUSB_DESC_ENDPOINT, PAD_EP_OUT, TUSB_XFER_INTERRUPT, 32, 0, out_iv,
            };
            memcpy(c + n, x, sizeof(x));
            n += sizeof(x);
        } else {
            const uint8_t h[] = {
                9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_HID, 0, 0, 0,
                9, HID_DESC_TYPE_HID, 0x11, 0x01, 0, 1, HID_DESC_TYPE_REPORT,
                (uint8_t)(m->report_desc_len & 0xFF), (uint8_t)(m->report_desc_len >> 8),
                7, TUSB_DESC_ENDPOINT, PAD_EP_IN, TUSB_XFER_INTERRUPT, PAD_EP_SIZE, 0, in_iv,
                7, TUSB_DESC_ENDPOINT, PAD_EP_OUT, TUSB_XFER_INTERRUPT, PAD_EP_SIZE, 0, in_iv,
            };
            memcpy(c + n, h, sizeof(h));
            n += sizeof(h);
        }
        if (s.audio) {
            const uint8_t *a = hs ? s_audio_hs : s_audio_fs;
            memcpy(c + n, a, USB_AUDIO_DESC_LEN);
            n += USB_AUDIO_DESC_LEN;
        }
        /* configuration: 1 or 3 interfaces, bus powered, 500 mA */
        const uint8_t head[9] = {9, TUSB_DESC_CONFIGURATION, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8),
                                 s.audio ? 3 : 1, 1, 0, 0x80, 0xFA};
        memcpy(c, head, sizeof(head));
    }
}

/* ---- the class driver --------------------------------------------------------------- */

static void pad_init(void)
{
}

static bool pad_deinit(void)
{
    return true;
}

static void pad_reset_cb(uint8_t rhport)
{
    s.open = false;
}

/* Send a pending reply, else the input report if it changed (or always, when streaming). */
static void try_send(void)
{
    if (!s.open || !tud_ready()) return;
    if (!usbd_edpt_claim(s.rhport, PAD_EP_IN)) return;
    size_t n = 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.reply_len) {
        n = s.reply_len;
        memcpy(s_buf.in, s.reply, n);
        s.reply_len = 0;
    } else if (s.dirty || pad_streaming(s.mode)) {
        n = pad_input_report(s.mode, &s.state, s_buf.in);
        s.dirty = false;
    }
    xSemaphoreGive(s.lock);
    if (!n || !usbd_edpt_xfer(s.rhport, PAD_EP_IN, s_buf.in, (uint16_t)n, false)) {
        usbd_edpt_release(s.rhport, PAD_EP_IN);
    }
}

static uint16_t pad_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    bool xinput = itf->bInterfaceClass == 0xFF && itf->bInterfaceSubClass == 0x5D && itf->bInterfaceProtocol == 0x01;
    bool hid = itf->bInterfaceClass == TUSB_CLASS_HID;
    if (!xinput && !hid) return 0;
    uint16_t len = tu_desc_len(itf);
    uint8_t const *p = tu_desc_next(itf);
    int opened = 0;
    while (opened < itf->bNumEndpoints && len < max_len) {
        if (tu_desc_type(p) == TUSB_DESC_INTERFACE) break;
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            if (!usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p)) return 0;
            opened++;
        }
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    s.rhport = rhport;
    s.open = true;
    usbd_edpt_xfer(rhport, PAD_EP_OUT, s_buf.out, PAD_EP_SIZE, false);
    ESP_LOGI(TAG, "%s interface opened by the host", pad_mode_info(s.mode)->name);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.dirty = true; /* the current state, so the host starts from it */
    xSemaphoreGive(s.lock);
    try_send();
    return len;
}

/* An output report, from interrupt OUT or SET_REPORT. */
static void on_output(const uint8_t *data, size_t len)
{
    static pad_output_t o; /* the USB task only */
    xSemaphoreTake(s.lock, portMAX_DELAY);
    pad_output(s.mode, &s.state, data, len, &o);
    if (o.reply_len) {
        memcpy(s.reply, o.reply, o.reply_len);
        s.reply_len = o.reply_len;
    }
    xSemaphoreGive(s.lock);
    if (o.rumble && (o.large != s.large || o.small != s.small)) {
        s.large = o.large, s.small = o.small;
        if (s.rumble_cb) s.rumble_cb(s.large, s.small, s.rumble_arg);
    }
    try_send();
}

static bool pad_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    const pad_mode_info_t *m = pad_mode_info(s.mode);
    if (!m->report_desc) return false; /* XInput: the XUSB vendor requests are optional, stall them */
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;

    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (req->bRequest != TUSB_REQ_GET_DESCRIPTOR) return false;
        if (stage != CONTROL_STAGE_SETUP) return true;
        uint8_t type = tu_u16_high(req->wValue);
        if (type == HID_DESC_TYPE_REPORT) {
            return tud_control_xfer(rhport, req, (void *)m->report_desc, m->report_desc_len);
        }
        if (type == HID_DESC_TYPE_HID) return tud_control_xfer(rhport, req, s.hid_desc, sizeof(s.hid_desc));
        return false;
    }
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;

    switch (req->bRequest) {
    case HID_REQ_CONTROL_GET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            uint8_t type = tu_u16_high(req->wValue), id = tu_u16_low(req->wValue);
            size_t n = 0;
            xSemaphoreTake(s.lock, portMAX_DELAY);
            if (type == HID_REPORT_TYPE_FEATURE) {
                n = pad_feature(s.mode, id, s.ctrl, sizeof(s.ctrl));
            } else if (type == HID_REPORT_TYPE_INPUT) {
                n = pad_input_report(s.mode, &s.state, s.ctrl);
            }
            xSemaphoreGive(s.lock);
            if (!n) {
                ESP_LOGI(TAG, "GET_REPORT type %u id 0x%02x: none", type, id);
                return false;
            }
            return tud_control_xfer(rhport, req, s.ctrl, (uint16_t)(n < req->wLength ? n : req->wLength));
        }
        return true;
    case HID_REQ_CONTROL_SET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            uint16_t n = req->wLength < sizeof(s.ctrl) ? req->wLength : sizeof(s.ctrl);
            return tud_control_xfer(rhport, req, s.ctrl, n);
        }
        if (stage == CONTROL_STAGE_DATA) {
            size_t n = req->wLength < sizeof(s.ctrl) ? req->wLength : sizeof(s.ctrl);
            on_output(s.ctrl, n);
        }
        return true;
    case HID_REQ_CONTROL_SET_IDLE:
    case HID_REQ_CONTROL_SET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_status(rhport, req);
        return true;
    case HID_REQ_CONTROL_GET_IDLE:
    case HID_REQ_CONTROL_GET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) {
            s.ctrl[0] = req->bRequest == HID_REQ_CONTROL_GET_PROTOCOL ? 1 : 0; /* report protocol, no idle rate */
            return tud_control_xfer(rhport, req, s.ctrl, 1);
        }
        return true;
    default: return false;
    }
}

static bool pad_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    if (ep_addr == PAD_EP_OUT) {
        if (result == XFER_RESULT_SUCCESS && xferred_bytes) {
            uint8_t data[PAD_EP_SIZE];
            memcpy(data, s_buf.out, xferred_bytes);
            usbd_edpt_xfer(rhport, PAD_EP_OUT, s_buf.out, PAD_EP_SIZE, false);
            on_output(data, xferred_bytes);
        } else {
            usbd_edpt_xfer(rhport, PAD_EP_OUT, s_buf.out, PAD_EP_SIZE, false);
        }
    } else if (ep_addr == PAD_EP_IN) {
        try_send();
    }
    return true;
}

static const usbd_class_driver_t s_pad_driver = {
    .name = "PAD",
    .init = pad_init,
    .deinit = pad_deinit,
    .reset = pad_reset_cb,
    .open = pad_open,
    .control_xfer_cb = pad_control_xfer_cb,
    .xfer_cb = pad_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    static usbd_class_driver_t drivers[2];
    drivers[0] = s_pad_driver;
    drivers[1] = usb_audio_driver;
    *driver_count = s.audio ? 2 : 1;
    return drivers;
}

/* ---- API ---------------------------------------------------------------------------- */

const char *usb_pad_mode_name(usb_pad_mode_t mode)
{
    return pad_mode_info(mode)->name;
}

usb_pad_mode_t usb_pad_mode(void)
{
    return s.mode;
}

void usb_pad_send(const usb_pad_state_t *st)
{
    if (!s.lock) return;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.state = *st;
    s.dirty = true;
    xSemaphoreGive(s.lock);
    try_send();
}

bool usb_pad_mounted(void)
{
    return s.open && tud_mounted();
}

static void on_event(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED: ESP_LOGI(TAG, "attached to a USB host"); break;
    case TINYUSB_EVENT_DETACHED: ESP_LOGI(TAG, "detached from the USB host"); break;
    case TINYUSB_EVENT_SUSPENDED: ESP_LOGI(TAG, "USB suspended"); break;
    case TINYUSB_EVENT_RESUMED: ESP_LOGI(TAG, "USB resumed"); break;
    default: break;
    }
}

static esp_err_t install(void)
{
    build_descriptors();
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(on_event);
    cfg.descriptor.device = &s.device;
    cfg.descriptor.qualifier = &s.qualifier;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    cfg.descriptor.full_speed_config = s.config_fs;
    cfg.descriptor.high_speed_config = s.config_hs;
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s gamepad%s on the high-speed USB-C port", pad_mode_info(s.mode)->name,
                 s.audio ? " and USB headphones (48 kHz stereo)" : "");
    } else {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t usb_pad_start(usb_pad_mode_t mode, usb_pad_rumble_cb_t rumble_cb, void *arg, bool audio)
{
    s.lock = xSemaphoreCreateMutex();
    s.config_fs = malloc(CONFIG_MAX);
    s.config_hs = malloc(CONFIG_MAX);
    if (!s.lock || !s.config_fs || !s.config_hs) return ESP_ERR_NO_MEM;
    s.mode = mode < USB_PAD_MODE_COUNT ? mode : USB_PAD_XINPUT;
    s.audio_ok = audio && usb_audio_init();
    s.rumble_cb = rumble_cb;
    s.rumble_arg = arg;
    return install();
}

esp_err_t usb_pad_set_mode(usb_pad_mode_t mode)
{
    if (mode >= USB_PAD_MODE_COUNT || mode == s.mode) return ESP_OK;
    ESP_LOGI(TAG, "switching to %s: leaving the bus", pad_mode_info(mode)->name);
    s.open = false;
    vTaskDelay(pdMS_TO_TICKS(20)); /* let a try_send() in flight finish */
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB uninstall failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200)); /* long enough for the host to see the device go */
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.mode = mode;
    s.reply_len = 0;
    s.dirty = true;
    pad_reset();
    xSemaphoreGive(s.lock);
    if (s.large || s.small) {
        s.large = s.small = 0;
        if (s.rumble_cb) s.rumble_cb(0, 0, s.rumble_arg);
    }
    return install();
}
