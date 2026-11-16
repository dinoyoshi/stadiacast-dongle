/*
 * DualShock 4 over USB: PC games and Steam (not a real PS4, which checks for Sony hardware).
 */
#include "pad_modes.h"
#include "pad_util.h"

#include <string.h>

#include "stadia_port.h"

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
        port_mac(mac);
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
static void ds4_output(const usb_pad_state_t *st, const uint8_t *d, size_t n, pad_output_t *o)
{
    if (n >= 6 && d[0] == 0x05 && (d[1] & 0x01)) {
        o->rumble = true;
        o->small = d[4], o->large = d[5];
    }
}

const pad_mode_t pad_mode_ds4 = {
    .info = {"DualShock 4", 0x054C, 0x09CC, 0x0100, s_ds4_desc, sizeof(s_ds4_desc), 4, 6, true, true, false},
    .report = ds4_report,
    .output = ds4_output,
    .feature = ds4_feature,
};
