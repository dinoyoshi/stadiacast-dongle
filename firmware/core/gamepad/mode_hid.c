/*
 * Plain HID gamepad: macOS, Android, older DirectInput PC games.
 */
#include "pad_modes.h"
#include "pad_util.h"

#include <string.h>

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

const pad_mode_t pad_mode_hid = {
    .info = {"HID gamepad", 0x303A, 0x4047, 0x0100, s_hid_desc, sizeof(s_hid_desc), 1, 4, false, true, true},
    .report = hid_report,
};
