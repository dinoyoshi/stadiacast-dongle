/*
 * The gamepad modes' reports and protocols (pad_modes.h).
 *
 * Buttons map by position, as on the Stadia controller: bottom = Xbox A = PlayStation cross =
 * Nintendo B, right = B / circle / A, left = X / square / Y, top = Y / triangle / X.
 */
#include "pad_modes.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "usb_pad";

/* ---- report descriptors ------------------------------------------------------------- */

/* Plain HID gamepad: 16 buttons, a hat, 4 x 16-bit sticks, 2 x 8-bit triggers (13 bytes). */
static const uint8_t s_hid_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,                                     /* Gamepad */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,             /* buttons 1-16 */
    0x75, 0x01, 0x95, 0x10, 0x81, 0x02,                                     /* */
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, /* hat */
    0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,                   /* */
    0x65, 0x00, 0x45, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,             /* padding */
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x80, 0x26, /* X Y Rx Ry */
    0xFF, 0x7F, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02,                         /* */
    0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, /* Z Rz: triggers */
    0x02, 0x81, 0x02,                                                       /* */
    0xC0,
};

/* DualShock 4 (USB): input report 1 (63 bytes), output report 5 (rumble, light bar), and the
 * feature reports the Linux and Steam drivers read when it is plugged in. */
static const uint8_t s_ds4_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,                         /* Gamepad, report 1 */
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, /* X Y Z Rz */
    0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,                               /* */
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, /* hat */
    0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,                               /* */
    0x65, 0x00, 0x45, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00, /* buttons 1-14 */
    0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02,                         /* */
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x15, 0x00, 0x25, /* counter */
    0x7F, 0x81, 0x02,                                                       /* */
    0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, /* Rx Ry: triggers */
    0x08, 0x95, 0x02, 0x81, 0x02,                                           /* */
    0x06, 0x00, 0xFF, 0x09, 0x21, 0x95, 0x36, 0x81, 0x02,                   /* the rest: 54 bytes */
    0x85, 0x05, 0x09, 0x22, 0x95, 0x1F, 0x91, 0x02,                         /* output 5: 31 bytes */
    0x85, 0x02, 0x09, 0x24, 0x95, 0x24, 0xB1, 0x02,                         /* feature 2: calibration */
    0x85, 0x12, 0x09, 0x25, 0x95, 0x0F, 0xB1, 0x02,                         /* feature 0x12: pairing */
    0x85, 0xA3, 0x09, 0x26, 0x95, 0x30, 0xB1, 0x02,                         /* feature 0xA3: firmware */
    0xC0,
};

/* Switch Pro Controller: the controller's own descriptor. */
static const uint8_t s_switch_desc[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0A,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0B,
    0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x01, 0x95, 0x02, 0x81, 0x03,
    0x0B, 0x01, 0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00, 0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B,
    0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01, 0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10,
    0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B, 0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B,
    0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0F, 0x29, 0x12, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03,
    0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04, 0x75, 0x08, 0x95, 0x3F,
    0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x82, 0x09, 0x06, 0x75, 0x08,
    0x95, 0x3F, 0x91, 0x83, 0xC0,
};

static const pad_mode_info_t s_modes[USB_PAD_MODE_COUNT] = {
    /* bInterval: full speed in frames (1 ms), high speed as 2^(n-1) microframes (4 -> 1 ms, 6 -> 4 ms, 7 -> 8 ms) */
    [USB_PAD_XINPUT] = {"Xbox 360", 0x045E, 0x028E, 0x0114, NULL, 0, 1, 4, false, true},
    [USB_PAD_DS4] = {"DualShock 4", 0x054C, 0x09CC, 0x0100, s_ds4_desc, sizeof(s_ds4_desc), 4, 6, true, true},
    [USB_PAD_SWITCH] = {"Switch Pro", 0x057E, 0x2009, 0x0200, s_switch_desc, sizeof(s_switch_desc), 8, 7, true, false},
    [USB_PAD_HID] = {"HID gamepad", 0x303A, 0x4047, 0x0100, s_hid_desc, sizeof(s_hid_desc), 1, 4, false, true},
};

const pad_mode_info_t *pad_mode_info(usb_pad_mode_t mode)
{
    return &s_modes[mode < USB_PAD_MODE_COUNT ? mode : USB_PAD_XINPUT];
}

/* ---- helpers ------------------------------------------------------------------------ */

/* HID hat: 0 = up, clockwise to 7 = up-left, 8 = centred. */
static uint8_t hat(uint16_t b)
{
    bool u = b & USB_PAD_UP, d = b & USB_PAD_DOWN, l = b & USB_PAD_LEFT, r = b & USB_PAD_RIGHT;
    if (u && r) return 1;
    if (d && r) return 3;
    if (d && l) return 5;
    if (u && l) return 7;
    if (u) return 0;
    if (r) return 2;
    if (d) return 4;
    if (l) return 6;
    return 8;
}

/* -32768..32767 (up positive) to 0..255 (down positive when flip) */
static uint8_t axis8(int16_t v, bool flip)
{
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(flip ? 255 - x : x);
}

static int16_t neg(int16_t v)
{
    return v == -32768 ? 32767 : (int16_t)-v;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF, p[1] = v >> 8;
}

/* ---- plain HID ---------------------------------------------------------------------- */

static size_t hid_report(const usb_pad_state_t *st, uint8_t *r)
{
    static const uint16_t order[] = {USB_PAD_A,    USB_PAD_B,     USB_PAD_X,  USB_PAD_Y,  USB_PAD_LB,
                                     USB_PAD_RB,   USB_PAD_BACK,  USB_PAD_START, USB_PAD_GUIDE, USB_PAD_LS,
                                     USB_PAD_RS};
    uint16_t b = 0;
    for (int i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
        if (st->buttons & order[i]) b |= 1u << i;
    }
    if (st->extra & USB_PAD_CAPTURE) b |= 1u << 11;
    if (st->extra & USB_PAD_ASSISTANT) b |= 1u << 12;
    put16(r, b);
    r[2] = hat(st->buttons);
    put16(r + 3, (uint16_t)st->lx);
    put16(r + 5, (uint16_t)neg(st->ly)); /* HID: down positive */
    put16(r + 7, (uint16_t)st->rx);
    put16(r + 9, (uint16_t)neg(st->ry));
    r[11] = st->lt, r[12] = st->rt;
    return 13;
}

/* ---- DualShock 4 -------------------------------------------------------------------- */

static struct {
    uint8_t counter;
    uint16_t timestamp;
} D;

static size_t ds4_report(const usb_pad_state_t *st, uint8_t *r)
{
    memset(r, 0, 64);
    r[0] = 0x01;
    r[1] = axis8(st->lx, false);
    r[2] = axis8(st->ly, true);
    r[3] = axis8(st->rx, false);
    r[4] = axis8(st->ry, true);
    uint16_t b = st->buttons;
    r[5] = hat(b) | (b & USB_PAD_X ? 0x10 : 0) | (b & USB_PAD_A ? 0x20 : 0) | (b & USB_PAD_B ? 0x40 : 0) |
           (b & USB_PAD_Y ? 0x80 : 0);
    r[6] = (b & USB_PAD_LB ? 0x01 : 0) | (b & USB_PAD_RB ? 0x02 : 0) | (st->lt > 30 ? 0x04 : 0) |
           (st->rt > 30 ? 0x08 : 0) | (b & USB_PAD_BACK ? 0x10 : 0) | (b & USB_PAD_START ? 0x20 : 0) |
           (b & USB_PAD_LS ? 0x40 : 0) | (b & USB_PAD_RS ? 0x80 : 0);
    r[7] = (b & USB_PAD_GUIDE ? 0x01 : 0) | (st->extra & USB_PAD_CAPTURE ? 0x02 : 0) | (uint8_t)(D.counter++ << 2);
    r[8] = st->lt, r[9] = st->rt;
    D.timestamp += 188; /* units of 5.33 us: about 1 ms */
    put16(r + 10, D.timestamp);
    r[12] = 0x14;
    put16(r + 21, 8192); /* accelerometer y: the controller lying flat (1 g) */
    r[30] = 0x1B;        /* cable connected, battery full */
    r[33] = 0x00;        /* no touch packets */
    r[35] = 0x80, r[39] = 0x80; /* both touch points released */
    return 64;
}

static size_t ds4_feature(uint8_t id, uint8_t *f, size_t cap)
{
    if (cap < 64) return 0;
    memset(f, 0, 64);
    f[0] = id;
    switch (id) {
    case 0x02: { /* motion calibration (USB layout): biases 0, +-8000 per axis, 540 deg/s, accel +-8192 */
        static const int16_t cal[] = {0, 0, 0, 8000, -8000, 8000, -8000, 8000, -8000,
                                      540, 540, 8192, -8192, 8192, -8192, 8192, -8192};
        for (int i = 0; i < (int)(sizeof(cal) / sizeof(cal[0])); i++) put16(f + 1 + 2 * i, (uint16_t)cal[i]);
        return 37;
    }
    case 0x12: { /* pairing info: the controller's MAC in bytes 1-6, little-endian */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        for (int i = 0; i < 6; i++) f[1 + i] = mac[5 - i];
        return 16;
    }
    case 0xA3: /* firmware info: build date and time strings, then versions */
        memcpy(f + 1, "Nov 15 2026", 11);
        memcpy(f + 17, "12:00:00", 8);
        put16(f + 35, 0x0100); /* hardware */
        put16(f + 41, 0x0100); /* firmware */
        return 49;
    default: return 0;
    }
}

/* Output report 5: [1] flags (bit 0 motors), [4] right (small) motor, [5] left (large) motor. */
static void ds4_output(const uint8_t *d, size_t n, pad_output_t *o)
{
    if (n >= 6 && d[0] == 0x05 && (d[1] & 0x01)) {
        o->rumble = true;
        o->small = d[4], o->large = d[5];
    }
}

/* ---- Switch Pro Controller ---------------------------------------------------------- */

static struct {
    bool streaming;
    uint8_t timer;
} N;

/* The controller's SPI flash, the parts the console and the Linux driver read. */
typedef struct {
    uint32_t addr;
    uint8_t len;
    const uint8_t *data;
} spi_block_t;

/* Sticks: 12-bit, centre 0x800, 0x600 either way. Left: above, centre, below; right: centre,
 * below, above. Each pair of 12-bit values packed into 3 bytes. */
#define PAIR(a, b) (a) & 0xFF, (((a) >> 8) & 0x0F) | (((b) & 0x0F) << 4), ((b) >> 4) & 0xFF
static const uint8_t s_stick_cal[] = {
    PAIR(0x600, 0x600), PAIR(0x800, 0x800), PAIR(0x600, 0x600), /* left */
    PAIR(0x800, 0x800), PAIR(0x600, 0x600), PAIR(0x600, 0x600), /* right */
};
static const uint8_t s_sensor_cal[] = {0xD3, 0xFF, 0xD5, 0xFF, 0x55, 0x01, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40,
                                       0x19, 0x00, 0xDD, 0xFF, 0xDC, 0xFF, 0x3B, 0x34, 0x3B, 0x34, 0x3B, 0x34};
static const uint8_t s_stick_params[] = {0x50, 0xFD, 0x00, 0x00, 0xC6, 0x0F, 0x0F, 0x30, 0x61, 0x96, 0x30, 0xF3,
                                         0xD4, 0x14, 0x54, 0x41, 0x15, 0x54, 0xC7, 0x79, 0x9C, 0x33, 0x36, 0x63};
/* body, buttons, left grip, right grip */
static const uint8_t s_colors[] = {0x32, 0x32, 0x32, 0xFF, 0xFF, 0xFF, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32};
static const uint8_t s_type[] = {0x03}; /* 0x6012: Pro Controller */
static const uint8_t s_color_info[] = {0x01}; /* 0x601B: colours from 0x6050 */

static const spi_block_t s_spi[] = {
    {0x6012, sizeof(s_type), s_type},
    {0x601B, sizeof(s_color_info), s_color_info},
    {0x6020, sizeof(s_sensor_cal), s_sensor_cal},
    {0x603D, sizeof(s_stick_cal), s_stick_cal},
    {0x6050, sizeof(s_colors), s_colors},
    {0x6080, sizeof(s_stick_params), s_stick_params},
    {0x6098, sizeof(s_stick_params) - 6, s_stick_params + 6},
};

static void spi_read(uint32_t addr, uint8_t len, uint8_t *out)
{
    memset(out, 0xFF, len); /* erased: no user calibration, no serial */
    for (int i = 0; i < (int)(sizeof(s_spi) / sizeof(s_spi[0])); i++) {
        const spi_block_t *b = &s_spi[i];
        for (uint32_t a = addr; a < addr + len; a++) {
            if (a >= b->addr && a < b->addr + b->len) out[a - addr] = b->data[a - b->addr];
        }
    }
}

static void pack12(uint8_t *p, int16_t x, int16_t y)
{
    uint16_t ux = (uint16_t)(0x800 + (int)x * 0x600 / 32768);
    uint16_t uy = (uint16_t)(0x800 + (int)y * 0x600 / 32768); /* up positive, as on the Pro */
    p[0] = ux & 0xFF;
    p[1] = ((ux >> 8) & 0x0F) | ((uy & 0x0F) << 4);
    p[2] = (uy >> 4) & 0xFF;
}

/* Bytes 1-12 of the 0x30 and 0x21 reports: timer, battery, buttons, sticks, vibrator. */
static void switch_state(const usb_pad_state_t *st, uint8_t *r)
{
    uint16_t b = st->buttons;
    r[1] = N.timer++;
    r[2] = 0x91; /* battery full, charging, USB powered */
    r[3] = (b & USB_PAD_X ? 0x01 : 0) | (b & USB_PAD_Y ? 0x02 : 0) | (b & USB_PAD_A ? 0x04 : 0) |
           (b & USB_PAD_B ? 0x08 : 0) | (b & USB_PAD_RB ? 0x40 : 0) | (st->rt > 30 ? 0x80 : 0);
    r[4] = (b & USB_PAD_BACK ? 0x01 : 0) | (b & USB_PAD_START ? 0x02 : 0) | (b & USB_PAD_RS ? 0x04 : 0) |
           (b & USB_PAD_LS ? 0x08 : 0) | (b & USB_PAD_GUIDE ? 0x10 : 0) | (st->extra & USB_PAD_CAPTURE ? 0x20 : 0);
    r[5] = (b & USB_PAD_DOWN ? 0x01 : 0) | (b & USB_PAD_UP ? 0x02 : 0) | (b & USB_PAD_RIGHT ? 0x04 : 0) |
           (b & USB_PAD_LEFT ? 0x08 : 0) | (b & USB_PAD_LB ? 0x40 : 0) | (st->lt > 30 ? 0x80 : 0);
    pack12(r + 6, st->lx, st->ly);
    pack12(r + 9, st->rx, st->ry);
    r[12] = 0x0B;
}

static size_t switch_report(const usb_pad_state_t *st, uint8_t *r)
{
    memset(r, 0, 64);
    r[0] = 0x30;
    switch_state(st, r);
    return 64;
}

/* HD rumble, 4 bytes per side: [1] bits 1-7 the high band's amplitude (0..0x64), [3] - 0x40
 * and [2] bit 7 the low band's. The louder band, scaled to 0..255. */
static uint8_t hd_amp(const uint8_t *s)
{
    int hf = s[1] >> 1;
    int lf = (((s[3] & 0x7F) - 0x40) << 1) | (s[2] >> 7);
    if (lf < 0) lf = 0;
    int a = hf > lf ? hf : lf;
    if (a > 0x64) a = 0x64;
    return (uint8_t)(a * 255 / 0x64);
}

static void switch_rumble(const uint8_t *d, pad_output_t *o)
{
    o->rumble = true;
    o->large = hd_amp(d);     /* left */
    o->small = hd_amp(d + 4); /* right */
}

static void switch_subcommand(const usb_pad_state_t *st, const uint8_t *d, size_t n, pad_output_t *o)
{
    uint8_t *r = o->reply;
    memset(r, 0, 64);
    r[0] = 0x21;
    switch_state(st, r);
    uint8_t cmd = n > 10 ? d[10] : 0;
    const uint8_t *arg = d + 11;
    r[13] = 0x80; /* ACK */
    r[14] = cmd;
    switch (cmd) {
    case 0x02: { /* device info: firmware 3.139, Pro Controller, MAC, colours from SPI */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_BASE);
        r[13] = 0x82;
        r[15] = 0x03, r[16] = 0x8B, r[17] = 0x03, r[18] = 0x02;
        memcpy(r + 19, mac, 6);
        r[25] = 0x01, r[26] = 0x01;
        break;
    }
    case 0x04: r[13] = 0x83; break; /* trigger times: zeros */
    case 0x10: {                    /* SPI flash read: address (le32), length */
        uint32_t addr = arg[0] | arg[1] << 8 | (uint32_t)arg[2] << 16 | (uint32_t)arg[3] << 24;
        uint8_t len = arg[4] > 0x1D ? 0x1D : arg[4];
        r[13] = 0x90;
        memcpy(r + 15, arg, 5);
        spi_read(addr, len, r + 20);
        break;
    }
    case 0x21: /* NFC/IR MCU configuration */
        r[13] = 0xA0;
        r[15] = 0x01, r[16] = 0x00, r[17] = 0xFF, r[18] = 0x00, r[19] = 0x03, r[20] = 0x00, r[21] = 0x05, r[22] = 0x01;
        r[63] = 0x5C;
        break;
    default: break; /* input mode, lights, IMU, vibration, low power...: a plain ACK */
    }
    o->reply_len = 64;
}

static void switch_output(const usb_pad_state_t *st, const uint8_t *d, size_t n, pad_output_t *o)
{
    if (n < 2) return;
    switch (d[0]) {
    case 0x80: /* USB commands */
        memset(o->reply, 0, 64);
        o->reply[0] = 0x81;
        o->reply[1] = d[1];
        switch (d[1]) {
        case 0x01: { /* status: controller type and MAC (reversed) */
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_BASE);
            o->reply[3] = 0x03;
            for (int i = 0; i < 6; i++) o->reply[4 + i] = mac[5 - i];
            o->reply_len = 64;
            break;
        }
        case 0x02: /* handshake */
        case 0x03: /* baud rate */
            o->reply_len = 64;
            break;
        case 0x04: /* HID only, no USB timeout: start streaming */
            if (!N.streaming) ESP_LOGI(TAG, "Switch Pro: the host started the input reports");
            N.streaming = true;
            break;
        case 0x05: N.streaming = false; break;
        default: break;
        }
        break;
    case 0x01: /* rumble and a subcommand */
        if (n >= 10) switch_rumble(d + 2, o);
        switch_subcommand(st, d, n, o);
        break;
    case 0x10: /* rumble only */
        if (n >= 10) switch_rumble(d + 2, o);
        break;
    default: break;
    }
}

/* ---- dispatch ----------------------------------------------------------------------- */

size_t pad_input_report(usb_pad_mode_t mode, const usb_pad_state_t *st, uint8_t *out)
{
    switch (mode) {
    case USB_PAD_DS4: return ds4_report(st, out);
    case USB_PAD_SWITCH: return switch_report(st, out);
    case USB_PAD_HID: return hid_report(st, out);
    default: break;
    }
    /* XInput: {0x00, 0x14, wButtons, LT, RT, LX, LY, RX, RY, 6 x 0} */
    memset(out, 0, 20);
    out[1] = 20;
    put16(out + 2, st->buttons);
    out[4] = st->lt, out[5] = st->rt;
    put16(out + 6, (uint16_t)st->lx);
    put16(out + 8, (uint16_t)st->ly);
    put16(out + 10, (uint16_t)st->rx);
    put16(out + 12, (uint16_t)st->ry);
    return 20;
}

void pad_output(usb_pad_mode_t mode, const usb_pad_state_t *st, const uint8_t *data, size_t len, pad_output_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (mode) {
    case USB_PAD_DS4: ds4_output(data, len, out); break;
    case USB_PAD_SWITCH: switch_output(st, data, len, out); break;
    case USB_PAD_XINPUT: /* {0x00, 0x08, 0x00, large, small, ...}; LED patterns {0x01, 0x03, n} are ignored */
        if (len >= 5 && data[0] == 0x00 && data[1] == 0x08) {
            out->rumble = true;
            out->large = data[3], out->small = data[4];
        }
        break;
    default: break;
    }
}

size_t pad_feature(usb_pad_mode_t mode, uint8_t id, uint8_t *out, size_t cap)
{
    return mode == USB_PAD_DS4 ? ds4_feature(id, out, cap) : 0;
}

bool pad_streaming(usb_pad_mode_t mode)
{
    return mode == USB_PAD_SWITCH ? N.streaming : pad_mode_info(mode)->stream;
}

void pad_reset(void)
{
    N.streaming = false;
}
