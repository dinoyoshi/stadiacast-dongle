/*
 * The gamepad as the host sees it, whatever the board and whatever the mode: the modes, the
 * button bits and the state a mode turns into its reports (core/gamepad). The board's USB
 * device code (on the ESP32-P4: esp32p4/components/usb_pad) sends those reports.
 *
 * Adding a mode: a value below (before USB_PAD_MODE_COUNT), a mode_<name>.c with its
 * pad_mode_t (pad_modes.h), and its entry in pad_modes.c's table.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USB_PAD_XINPUT,
    USB_PAD_DS4,
    USB_PAD_SWITCH,
    USB_PAD_HID,
    USB_PAD_MODE_COUNT,
} usb_pad_mode_t;

/* usb_pad_state_t.buttons: the XInput wButtons bits, whatever the mode */
#define USB_PAD_UP 0x0001
#define USB_PAD_DOWN 0x0002
#define USB_PAD_LEFT 0x0004
#define USB_PAD_RIGHT 0x0008
#define USB_PAD_START 0x0010
#define USB_PAD_BACK 0x0020
#define USB_PAD_LS 0x0040
#define USB_PAD_RS 0x0080
#define USB_PAD_LB 0x0100
#define USB_PAD_RB 0x0200
#define USB_PAD_GUIDE 0x0400
#define USB_PAD_A 0x1000 /* bottom */
#define USB_PAD_B 0x2000 /* right */
#define USB_PAD_X 0x4000 /* left */
#define USB_PAD_Y 0x8000 /* top */
/* usb_pad_state_t.extra: buttons XInput has no bit for */
#define USB_PAD_CAPTURE 0x01
#define USB_PAD_ASSISTANT 0x02

typedef struct {
    uint16_t buttons; /* USB_PAD_* (XInput wButtons) */
    uint8_t extra;    /* USB_PAD_CAPTURE, USB_PAD_ASSISTANT */
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry; /* Y positive = up */
} usb_pad_state_t;

#define USB_PAD_MAX_PLAYERS 4
