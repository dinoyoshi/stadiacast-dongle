/*
 * Nintendo Switch Pro Controller over USB: the Switch, Steam, Linux. Its USB protocol (the 0x80
 * commands, subcommands, SPI flash reads) is answered here.
 */
#include "pad_modes.h"
#include "pad_util.h"

#include <string.h>

#include "stadia_port.h"

static const char *TAG = "usb_pad";

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
    int lf = (((s[3] & 0x7F) - 0x40) * 2) | (s[2] >> 7); /* * 2: the shift, without shifting a negative */
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
        port_mac(mac);
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
            port_mac(mac);
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
            if (!N.streaming) SLOGI(TAG, "Switch Pro: the host started the input reports");
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

static bool switch_streaming(void)
{
    return N.streaming;
}

static void switch_reset(void)
{
    N.streaming = false;
}

const pad_mode_t pad_mode_switch = {
    .info = {"Switch Pro", 0x057E, 0x2009, 0x0200, s_switch_desc, sizeof(s_switch_desc), 8, 7, true, false, false},
    .report = switch_report,
    .output = switch_output,
    .streaming = switch_streaming,
    .reset = switch_reset,
};
