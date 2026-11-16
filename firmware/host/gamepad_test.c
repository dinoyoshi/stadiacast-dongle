/*
 * The gamepad modes' reports, checked against what the board sent when they were last tested
 * on hosts: for each mode, a fixed series of pad states and host output reports (rumble, the
 * Switch's USB commands and subcommands, the DS4's feature reports) goes through pad_modes.h
 * and everything that comes back is hashed. A changed hash means a mode now sends something
 * else to the host: fine when meant (update the hash below, and test that mode on a host), a
 * bug when not.
 *
 *   gamepad_test          check
 *   gamepad_test -v       print each mode's hash (for a new mode, or a meant change)
 */
#include <stdio.h>
#include <string.h>

#include "pad_modes.h"

static const uint32_t s_expect[USB_PAD_MODE_COUNT] = {
    [USB_PAD_XINPUT] = 0x65A06012,
    [USB_PAD_DS4] = 0x1794DD06,
    [USB_PAD_SWITCH] = 0xA12C8871,
    [USB_PAD_HID] = 0x5783D241,
};

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

static uint32_t s_h;
static void hash(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) s_h = (s_h ^ b[i]) * 16777619u;
}
static void hash32(uint32_t v)
{
    hash(&v, sizeof(v));
}

static void out_report(usb_pad_mode_t m, const usb_pad_state_t *st, const uint8_t *d, size_t n)
{
    pad_output_t o;
    pad_output(m, st, d, n, &o);
    hash32((uint32_t)o.reply_len);
    hash(o.reply, o.reply_len);
    hash32(o.rumble);
    if (o.rumble) hash32(o.large << 8 | o.small);
}

static uint32_t run(usb_pad_mode_t m)
{
    s_h = 2166136261u;
    s_rng = 12345;
    pad_reset();
    const pad_mode_info_t *info = pad_mode_info(m);
    hash(info->name, strlen(info->name));
    hash32(info->vid << 16 | info->pid);
    hash32(info->bcd);
    hash(info->report_desc ? info->report_desc : (const uint8_t *)"", info->report_desc_len);
    hash32(info->interval_fs << 8 | info->interval_hs);
    hash32(info->stream << 2 | info->audio_ok << 1 | info->multi);

    uint8_t f[64];
    const uint8_t ids[] = {0x00, 0x02, 0x12, 0xA3, 0x05};
    for (size_t i = 0; i < sizeof(ids); i++) {
        size_t n = pad_feature(m, ids[i], f, sizeof(f));
        hash32((uint32_t)n);
        hash(f, n);
    }

    /* the commands each mode's hosts send */
    static const uint8_t cmds[][16] = {
        {0x00, 0x08, 0x00, 200, 50},                            /* XInput rumble */
        {0x01, 0x03, 0x02},                                     /* XInput LEDs */
        {0x05, 0x01, 0, 0, 90, 180},                            /* DS4 rumble */
        {0x05, 0x02, 0, 0, 90, 180},                            /* DS4 light bar only */
        {0x80, 0x01}, {0x80, 0x02}, {0x80, 0x03}, {0x80, 0x04}, /* Switch USB commands */
        {0x01, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x02},             /* device info */
        {0x01, 1, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x10, 0x3D, 0x60, 0, 0, 0x12}, /* SPI */
        {0x01, 2, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x21},             /* MCU */
        {0x01, 3, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x04},             /* trigger times */
        {0x01, 4, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x30, 0x01},       /* lights */
        {0x10, 5, 0x00, 0xC8, 0x80, 0x50, 0x00, 0x20, 0x62, 0x60},                  /* HD rumble */
    };
    usb_pad_state_t st = {0};
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        out_report(m, &st, cmds[i], 16);
        hash32(pad_streaming(m));
    }

    uint8_t r[PAD_REPORT_MAX];
    for (int i = 0; i < 3000; i++) {
        st.buttons = (uint16_t)rnd();
        st.extra = (uint8_t)(rnd() & 3);
        st.lt = (uint8_t)rnd(), st.rt = (uint8_t)rnd();
        st.lx = (int16_t)rnd(), st.ly = (int16_t)rnd(), st.rx = (int16_t)rnd(), st.ry = (int16_t)rnd();
        if (i % 50 == 0) st.lx = -32768, st.ly = 32767, st.ry = -32768;
        size_t n = pad_input_report(m, &st, r);
        hash32((uint32_t)n);
        hash(r, n);
        if (i % 7 == 0) { /* random output reports, starting with each report ID the modes know */
            uint8_t d[64];
            size_t len = rnd() % sizeof(d);
            for (size_t k = 0; k < len; k++) d[k] = (uint8_t)rnd();
            static const uint8_t first[] = {0x00, 0x01, 0x05, 0x10, 0x80};
            if (len) d[0] = first[rnd() % sizeof(first)];
            out_report(m, &st, d, len);
            hash32(pad_streaming(m));
        }
    }
    return s_h;
}

int main(int argc, char **argv)
{
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    int bad = 0;
    for (int m = 0; m < USB_PAD_MODE_COUNT; m++) {
        uint32_t h = run((usb_pad_mode_t)m);
        bool ok = h == s_expect[m];
        if (verbose || !ok) {
            printf("%-12s 0x%08X%s\n", pad_mode_info((usb_pad_mode_t)m)->name, h, ok ? "" : "  (expected another hash)");
        }
        bad += !ok;
    }
    printf("gamepad modes: %s\n", bad ? "CHANGED" : "as before");
    return bad ? 1 : 0;
}
