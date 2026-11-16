/*
 * The gamepad modes (pad_modes.h): the table of modes, one mode_<name>.c each, and the calls the
 * USB code makes, passed on to the current mode.
 *
 * Buttons map by position, as on the Stadia controller: bottom = Xbox A = PlayStation cross =
 * Nintendo B, right = B / circle / A, left = X / square / Y, top = Y / triangle / X.
 */
#include "pad_modes.h"

#include <string.h>

static const pad_mode_t *const s_modes[USB_PAD_MODE_COUNT] = {
    [USB_PAD_XINPUT] = &pad_mode_xinput,
    [USB_PAD_DS4] = &pad_mode_ds4,
    [USB_PAD_SWITCH] = &pad_mode_switch,
    [USB_PAD_HID] = &pad_mode_hid,
};

static const pad_mode_t *mode_of(usb_pad_mode_t mode)
{
    return s_modes[mode < USB_PAD_MODE_COUNT ? mode : USB_PAD_XINPUT];
}

const pad_mode_info_t *pad_mode_info(usb_pad_mode_t mode)
{
    return &mode_of(mode)->info;
}

size_t pad_input_report(usb_pad_mode_t mode, const usb_pad_state_t *st, uint8_t *out)
{
    return mode_of(mode)->report(st, out);
}

void pad_output(usb_pad_mode_t mode, const usb_pad_state_t *st, const uint8_t *data, size_t len, pad_output_t *out)
{
    memset(out, 0, sizeof(*out));
    const pad_mode_t *m = mode_of(mode);
    if (m->output) m->output(st, data, len, out);
}

size_t pad_feature(usb_pad_mode_t mode, uint8_t id, uint8_t *out, size_t cap)
{
    const pad_mode_t *m = mode_of(mode);
    return m->feature ? m->feature(id, out, cap) : 0;
}

bool pad_streaming(usb_pad_mode_t mode)
{
    const pad_mode_t *m = mode_of(mode);
    return m->streaming ? m->streaming() : m->info.stream;
}

void pad_reset(void)
{
    for (int i = 0; i < USB_PAD_MODE_COUNT; i++) {
        if (s_modes[i]->reset) s_modes[i]->reset();
    }
}
