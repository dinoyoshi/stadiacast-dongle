/*
 * What each gamepad mode sends and answers, apart from USB itself (usb_pad.c): the HID report
 * descriptors, the input reports, the DualShock 4's feature reports and the Switch Pro
 * Controller's USB protocol.
 */
#pragma once

#include "usb_pad.h"

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

/* An output report from the host (interrupt OUT or SET_REPORT), data[0] being its report ID. */
void pad_output(usb_pad_mode_t mode, const usb_pad_state_t *st, const uint8_t *data, size_t len, pad_output_t *out);
/* A GET_REPORT for a feature report; 0 when there is none. */
size_t pad_feature(usb_pad_mode_t mode, uint8_t id, uint8_t *out, size_t cap);
/* The Switch sends input reports only after the host's 0x80 0x04. */
bool pad_streaming(usb_pad_mode_t mode);
void pad_reset(void);
