/*
 * Wired Xbox 360 controller (XInput): Windows, Linux, SteamOS, Android. A vendor-class interface,
 * no HID report descriptor: the USB code builds its XUSB descriptors (esp32p4/components/usb_pad).
 */
#include "pad_modes.h"
#include "pad_util.h"

#include <string.h>

/* {0x00, 0x14, wButtons, LT, RT, LX, LY, RX, RY, 6 x 0} */
static size_t xinput_report(const usb_pad_state_t *st, uint8_t *out)
{
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

/* {0x00, 0x08, 0x00, large, small, ...}; LED patterns {0x01, 0x03, n} are ignored */
static void xinput_output(const usb_pad_state_t *st, const uint8_t *data, size_t len, pad_output_t *out)
{
    if (len >= 5 && data[0] == 0x00 && data[1] == 0x08) {
        out->rumble = true;
        out->large = data[3], out->small = data[4];
    }
}

const pad_mode_t pad_mode_xinput = {
    .info = {"Xbox 360", 0x045E, 0x028E, 0x0114, NULL, 0, 1, 4, false, true, true},
    .report = xinput_report,
    .output = xinput_output,
};
