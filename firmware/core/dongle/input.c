/* The controller's input as the USB gamepad's state (dongle_core.h). */
#include "dongle_core.h"

/* XInput wButtons (usb_pad_state_t.buttons in every mode) share bits 0-9 and 12-15 with the
 * controller's layout; the Stadia button is the guide button, Assistant and Capture go in extra. */
#define XINPUT_SHARED_BITS 0xF3FF

void dongle_pad_state(const stadia_pad_t *pad, usb_pad_state_t *st)
{
    *st = (usb_pad_state_t){0}; /* a disconnect releases everything */
    if (!pad->connected) return;
    st->buttons = (pad->buttons & XINPUT_SHARED_BITS) | ((pad->buttons & STADIA_BTN_STADIA) ? USB_PAD_GUIDE : 0);
    st->extra = (pad->buttons & STADIA_BTN_CAPTURE ? USB_PAD_CAPTURE : 0) |
                (pad->buttons & STADIA_BTN_ASSISTANT ? USB_PAD_ASSISTANT : 0);
    st->lt = pad->lt, st->rt = pad->rt;
    st->lx = pad->lx, st->ly = pad->ly, st->rx = pad->rx, st->ry = pad->ry;
}
