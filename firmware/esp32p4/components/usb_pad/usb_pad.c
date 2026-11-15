/*
 * XInput (wired Xbox 360 controller) as a TinyUSB application class driver.
 *
 * The host's XUSB / xpad drivers bind by VID:PID 045E:028E and the vendor interface
 * FF/5D/01. Input goes out as the 20-byte report {0x00, 0x14, wButtons, LT, RT, LX, LY, RX,
 * RY, 6 x 0} on interrupt IN 0x81; rumble comes back as {0x00, 0x08, 0x00, large, small, ...}
 * on interrupt OUT 0x01 (LED patterns {0x01, 0x03, n} are ignored).
 *
 * With audio the device is composite (class EF/02/01): XInput stays interface 0 and the UAC2
 * headphones of usb_audio.c are interfaces 1 and 2, behind an interface association.
 */
#include "usb_pad.h"
#include "usb_audio.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "device/usbd_pvt.h"

static const char *TAG = "usb_pad";

#define XINPUT_EP_IN 0x81
#define XINPUT_EP_OUT 0x01
#define XINPUT_EP_SIZE 32
#define XINPUT_REPORT_LEN 20

/* ---- descriptors -------------------------------------------------------------------- */

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xFF,
    .bDeviceSubClass = 0xFF,
    .bDeviceProtocol = 0xFF,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x045E, /* Microsoft */
    .idProduct = 0x028E, /* Xbox 360 Controller (wired) */
    .bcdDevice = 0x0114,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/* the same, as a composite device with the sound card */
static const tusb_desc_device_t s_device_audio = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x045E,
    .idProduct = 0x028E,
    .bcdDevice = 0x0114,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qualifier_audio = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xFF,
    .bDeviceSubClass = 0xFF,
    .bDeviceProtocol = 0xFF,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

#define XINPUT_ITF_LEN (9 + 17 + 7 + 7)
#define XINPUT_CONFIG_LEN (9 + XINPUT_ITF_LEN)
#define AUDIO_CONFIG_LEN (XINPUT_CONFIG_LEN + USB_AUDIO_DESC_LEN)

/* configuration: n interfaces, bus powered, 500 mA */
#define CONFIG_HEADER(total, nitf) 9, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(total), nitf, 1, 0, 0x80, 0xFA

/* bInterval: full speed in frames (1 ms), high speed as 2^(n-1) microframes (4 -> 1 ms, 7 -> 8 ms). */
#define XINPUT_ITF(in_interval, out_interval)                                                                         \
        /* interface 0: vendor FF/5D/01 with two endpoints */                                                         \
        9, TUSB_DESC_INTERFACE, 0, 0, 2, 0xFF, 0x5D, 0x01, 0,                                                       \
        /* the XUSB "unknown" descriptor (type 0x21) every wired 360 controller carries */                            \
        17, 0x21, 0x00, 0x01, 0x01, 0x25, XINPUT_EP_IN, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, XINPUT_EP_OUT, 0x08,     \
        0x00, 0x00,                                                                                                   \
        /* interrupt IN, then interrupt OUT */                                                                        \
        7, TUSB_DESC_ENDPOINT, XINPUT_EP_IN, TUSB_XFER_INTERRUPT, XINPUT_EP_SIZE, 0, (in_interval),                  \
        7, TUSB_DESC_ENDPOINT, XINPUT_EP_OUT, TUSB_XFER_INTERRUPT, XINPUT_EP_SIZE, 0, (out_interval)

static const uint8_t s_config_fs[] = {CONFIG_HEADER(XINPUT_CONFIG_LEN, 1), XINPUT_ITF(1, 8)};
static const uint8_t s_config_hs[] = {CONFIG_HEADER(XINPUT_CONFIG_LEN, 1), XINPUT_ITF(4, 7)};
static const uint8_t s_config_audio_fs[] = {CONFIG_HEADER(AUDIO_CONFIG_LEN, 3), XINPUT_ITF(1, 8), USB_AUDIO_DESC(1, USB_AUDIO_EP_SIZE_FS)};
static const uint8_t s_config_audio_hs[] = {CONFIG_HEADER(AUDIO_CONFIG_LEN, 3), XINPUT_ITF(4, 7), USB_AUDIO_DESC(1, USB_AUDIO_EP_SIZE_HS)};
_Static_assert(sizeof(s_config_fs) == XINPUT_CONFIG_LEN, "gamepad configuration length");
_Static_assert(sizeof(s_config_audio_hs) == AUDIO_CONFIG_LEN, "composite configuration length");

static const char *s_strings[] = {
    (const char[]){0x09, 0x04}, /* English */
    "Stadiacast",
    "Stadiacast Dongle",
    NULL, /* serial: from the chip's MAC */
    "Controller headset",
};

/* ---- state -------------------------------------------------------------------------- */

typedef struct {
    TUD_EPBUF_DEF(in, XINPUT_EP_SIZE);
    TUD_EPBUF_DEF(out, XINPUT_EP_SIZE);
} xinput_epbuf_t;

CFG_TUD_MEM_SECTION static xinput_epbuf_t s_buf;

static struct {
    uint8_t rhport;
    bool open;
    SemaphoreHandle_t lock;
    uint8_t report[XINPUT_REPORT_LEN];
    bool dirty;
    usb_pad_rumble_cb_t rumble_cb;
    void *rumble_arg;
    uint8_t large, small;
} s;

/* ---- the class driver --------------------------------------------------------------- */

static void xinput_init(void)
{
}

static bool xinput_deinit(void)
{
    return true;
}

static void xinput_reset(uint8_t rhport)
{
    s.open = false;
}

/* Send the pending report if the IN endpoint is free. */
static void try_send(void)
{
    if (!s.open || !tud_ready()) return;
    if (!usbd_edpt_claim(s.rhport, XINPUT_EP_IN)) return;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool dirty = s.dirty;
    if (dirty) {
        memcpy(s_buf.in, s.report, XINPUT_REPORT_LEN);
        s.dirty = false;
    }
    xSemaphoreGive(s.lock);
    if (!dirty || !usbd_edpt_xfer(s.rhport, XINPUT_EP_IN, s_buf.in, XINPUT_REPORT_LEN, false)) {
        usbd_edpt_release(s.rhport, XINPUT_EP_IN);
    }
}

static uint16_t xinput_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != 0xFF || itf->bInterfaceSubClass != 0x5D || itf->bInterfaceProtocol != 0x01) {
        return 0;
    }
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
    usbd_edpt_xfer(rhport, XINPUT_EP_OUT, s_buf.out, XINPUT_EP_SIZE, false);
    ESP_LOGI(TAG, "XInput interface opened by the host");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.dirty = true; /* the current state, so the host starts from it */
    xSemaphoreGive(s.lock);
    try_send();
    return len;
}

/* The XUSB driver's vendor requests (capabilities, serial) are optional: stall them. */
static bool xinput_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    return false;
}

static bool xinput_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
    if (ep_addr == XINPUT_EP_OUT) {
        const uint8_t *b = s_buf.out;
        if (result == XFER_RESULT_SUCCESS && xferred_bytes >= 5 && b[0] == 0x00 && b[1] == 0x08) {
            if (b[3] != s.large || b[4] != s.small) {
                s.large = b[3], s.small = b[4];
                if (s.rumble_cb) s.rumble_cb(s.large, s.small, s.rumble_arg);
            }
        }
        usbd_edpt_xfer(rhport, XINPUT_EP_OUT, s_buf.out, XINPUT_EP_SIZE, false);
    } else if (ep_addr == XINPUT_EP_IN) {
        try_send();
    }
    return true;
}

static const usbd_class_driver_t s_xinput = {
    .name = "XINPUT",
    .init = xinput_init,
    .deinit = xinput_deinit,
    .reset = xinput_reset,
    .open = xinput_open,
    .control_xfer_cb = xinput_control_xfer_cb,
    .xfer_cb = xinput_xfer_cb,
    .sof = NULL,
};
static bool s_audio;

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    static usbd_class_driver_t drivers[2];
    drivers[0] = s_xinput;
    drivers[1] = usb_audio_driver;
    *driver_count = s_audio ? 2 : 1;
    return drivers;
}

/* ---- API ---------------------------------------------------------------------------- */

void usb_pad_send(const usb_pad_state_t *st)
{
    uint8_t r[XINPUT_REPORT_LEN] = {0x00, XINPUT_REPORT_LEN};
    r[2] = st->buttons & 0xFF, r[3] = st->buttons >> 8;
    r[4] = st->lt, r[5] = st->rt;
    const int16_t axes[4] = {st->lx, st->ly, st->rx, st->ry};
    for (int i = 0; i < 4; i++) {
        r[6 + 2 * i] = (uint16_t)axes[i] & 0xFF;
        r[7 + 2 * i] = (uint16_t)axes[i] >> 8;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    memcpy(s.report, r, sizeof(r));
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

esp_err_t usb_pad_start(usb_pad_rumble_cb_t rumble_cb, void *arg, bool audio)
{
    s_audio = audio && usb_audio_init();
    s.lock = xSemaphoreCreateMutex();
    if (!s.lock) return ESP_ERR_NO_MEM;
    s.rumble_cb = rumble_cb;
    s.rumble_arg = arg;
    s.report[1] = XINPUT_REPORT_LEN;
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(on_event);
    cfg.descriptor.device = s_audio ? &s_device_audio : &s_device;
    cfg.descriptor.qualifier = s_audio ? &s_qualifier_audio : &s_qualifier;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    cfg.descriptor.full_speed_config = s_audio ? s_config_audio_fs : s_config_fs;
    cfg.descriptor.high_speed_config = s_audio ? s_config_audio_hs : s_config_hs;
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Xbox 360 style gamepad%s on the high-speed USB-C port",
                 s_audio ? " and USB headphones (48 kHz stereo)" : "");
    } else {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
    }
    return err;
}
