/*
 * What each gamepad mode sends and answers, apart from USB itself (the board's USB device code):
 * the HID report descriptors, the input reports, the DualShock 4's feature reports and the
 * Switch Pro Controller's USB protocol. Plain C, no board code: one mode_<name>.c per mode.
 */
#pragma once

#include "gamepad.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAD_REPORT_MAX 64

typedef struct {
    const char *name;
    uint16_t vid, pid, bcd;
    const uint8_t *report_desc; /* NULL: XInput (vendor class) */
    uint16_t report_desc_len;
    uint8_t interval_fs, interval_hs; /* both endpoints */
    bool stream;   /* the input report goes out every interval, not only on a change */
    bool audio_ok; /* the USB headphones may sit next to it */
    bool multi;    /* several gamepads of this kind can share the device (one interface each) */
} pad_mode_info_t;

const pad_mode_info_t *pad_mode_info(usb_pad_mode_t mode);

/* The input report for the current state (the Switch's 0x30 report once streaming is on). */
size_t pad_input_report(usb_pad_mode_t mode, const usb_pad_state_t *st, uint8_t *out);

/* What an output report (interrupt OUT or SET_REPORT) asks for. */
typedef struct {
    size_t reply_len; /* a report to send before the next input report (Switch) */
    uint8_t reply[PAD_REPORT_MAX];
    bool rumble; /* large/small are new rumble levels */
    uint8_t large, small;
} pad_output_t;

/* One mode: its USB identity and its reports. Only report is required. */
typedef struct {
    pad_mode_info_t info;
    size_t (*report)(const usb_pad_state_t *st, uint8_t *out); /* the input report, PAD_REPORT_MAX at most */
    /* an output report (interrupt OUT or SET_REPORT), data[0] being its report ID; out is zeroed.
     * NULL: the mode ignores them */
    void (*output)(const usb_pad_state_t *st, const uint8_t *data, size_t len, pad_output_t *out);
    size_t (*feature)(uint8_t id, uint8_t *out, size_t cap); /* GET_REPORT of a feature report; NULL: none */
    bool (*streaming)(void); /* whether input reports go out every interval; NULL: info.stream */
    void (*reset)(void);     /* the host went away: back to the state at plug-in; NULL: nothing */
} pad_mode_t;

extern const pad_mode_t pad_mode_xinput, pad_mode_ds4, pad_mode_switch, pad_mode_hid;

/* An output report from the host (interrupt OUT or SET_REPORT), data[0] being its report ID. */
void pad_output(usb_pad_mode_t mode, const usb_pad_state_t *st, const uint8_t *data, size_t len, pad_output_t *out);
/* A GET_REPORT for a feature report; 0 when there is none. */
size_t pad_feature(usb_pad_mode_t mode, uint8_t id, uint8_t *out, size_t cap);
/* The Switch sends input reports only after the host's 0x80 0x04. */
bool pad_streaming(usb_pad_mode_t mode);
void pad_reset(void);
