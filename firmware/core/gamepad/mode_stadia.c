/*
 * A wired Stadia Controller (Google 18D1:9400): Linux's own Stadia support takes it as it is,
 * the kernel's hid-google-stadiaff for rumble, and SDL's and Steam's Stadia drivers for the
 * buttons and the rumble. Also a plain HID gamepad anywhere else. bcdDevice 1.11 as the real
 * one, so that SDL's controller database knows it on Linux (03000000d11800000094000011010000).
 */
#include "pad_modes.h"
#include "pad_util.h"

#include <string.h>

/* The real controller's layout over USB. Input report 3: a hat, 15 buttons, the sticks
 * (X Y left, Z Rz right, 1..255 with 128 centred) and the triggers (brake = left, accelerator =
 * right). Output report 5: rumble, two 16-bit levels (strong = left motor, weak = right), what
 * hid-google-stadiaff and SDL send. */
static const uint8_t s_stadia_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03,                         /* Gamepad, report 3 */
    0x05, 0x01, 0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, /* hat */
    0x46, 0x3B, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42,                   /* */
    0x45, 0x00, 0x65, 0x00, 0x75, 0x01, 0x95, 0x04, 0x81, 0x01,             /* padding */
    0x05, 0x09, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0F,             /* 15 buttons, */
    0x09, 0x12, 0x09, 0x11, 0x09, 0x14, 0x09, 0x13, 0x09, 0x0D, 0x09, 0x0C, /* in the real */
    0x09, 0x0B, 0x09, 0x0F, 0x09, 0x0E, 0x09, 0x08, 0x09, 0x07, 0x09, 0x05, /* controller's */
    0x09, 0x04, 0x09, 0x02, 0x09, 0x01, 0x81, 0x02,                         /* order */
    0x75, 0x01, 0x95, 0x01, 0x81, 0x01,                                     /* padding */
    0x05, 0x01, 0x15, 0x01, 0x26, 0xFF, 0x00,                               /* sticks 1..255 */
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x75, 0x08, 0x95, 0x02, /* X Y */
    0x81, 0x02, 0xC0,                                                       /* */
    0x09, 0x01, 0xA1, 0x00, 0x09, 0x32, 0x09, 0x35, 0x75, 0x08, 0x95, 0x02, /* Z Rz */
    0x81, 0x02, 0xC0,                                                       /* */
    0x05, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02,       /* brake, accelerator */
    0x09, 0xC5, 0x09, 0xC4, 0x81, 0x02,                                     /* */
    0x85, 0x05, 0x06, 0x0F, 0x00, 0x09, 0x97, 0x15, 0x00, 0x27, 0xFF, 0xFF, /* output 5: rumble */
    0x00, 0x00, 0x75, 0x10, 0x95, 0x02, 0x91, 0x02,                         /* */
    0xC0,
};

/* -32768..32767 (up positive) to 1..255 with 0 at exactly 128 (down positive when flip), as the
 * real controller sends and SDL reads */
static uint8_t stick(int16_t v, bool flip)
{
    int x = v == -32768 ? -32767 : v;
    if (flip) x = -x;
    return (uint8_t)(128 + (x * 127 + (x < 0 ? -16383 : 16383)) / 32767);
}

static size_t stadia_report(const usb_pad_state_t *st, uint8_t *r)
{
    uint16_t b = st->buttons;
    r[0] = 0x03;
    r[1] = hat(b);
    r[2] = (st->extra & USB_PAD_CAPTURE ? 0x01 : 0) | (st->extra & USB_PAD_ASSISTANT ? 0x02 : 0) |
           (st->rt > 30 ? 0x04 : 0) | (st->lt > 30 ? 0x08 : 0) | (b & USB_PAD_GUIDE ? 0x10 : 0) |
           (b & USB_PAD_START ? 0x20 : 0) | (b & USB_PAD_BACK ? 0x40 : 0) | (b & USB_PAD_RS ? 0x80 : 0);
    r[3] = (b & USB_PAD_LS ? 0x01 : 0) | (b & USB_PAD_RB ? 0x02 : 0) | (b & USB_PAD_LB ? 0x04 : 0) |
           (b & USB_PAD_Y ? 0x08 : 0) | (b & USB_PAD_X ? 0x10 : 0) | (b & USB_PAD_B ? 0x20 : 0) |
           (b & USB_PAD_A ? 0x40 : 0);
    r[4] = stick(st->lx, false);
    r[5] = stick(st->ly, true);
    r[6] = stick(st->rx, false);
    r[7] = stick(st->ry, true);
    r[8] = st->lt, r[9] = st->rt;
    return 10;
}

/* Output report 5: [1..2] strong (left) motor, [3..4] weak (right) motor, little-endian 0..65535. */
static void stadia_output(const usb_pad_state_t *st, const uint8_t *d, size_t n, pad_output_t *o)
{
    if (n >= 5 && d[0] == 0x05) {
        o->rumble = true;
        o->large = d[2], o->small = d[4]; /* the high bytes */
    }
}

const pad_mode_t pad_mode_stadia = {
    .info = {"Stadia", 0x18D1, 0x9400, 0x0111, s_stadia_desc, sizeof(s_stadia_desc), 1, 4, false, true, true},
    .report = stadia_report,
    .output = stadia_output,
};
