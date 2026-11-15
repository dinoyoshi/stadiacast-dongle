/*
 * The gamepad on the P4's high-speed USB-C port, in one of several modes the host knows without
 * a driver:
 *   Xbox 360    wired Xbox 360 controller (XInput): Windows, Linux, SteamOS, Android
 *   DualShock 4 a PS4 controller over USB: PC games and Steam (not a real PS4, which checks
 *               for Sony hardware)
 *   Switch Pro  a Nintendo Switch Pro Controller over USB: the Switch, Steam, Linux
 *   HID         a plain HID gamepad: macOS, Android, older DirectInput PC games
 * The mode can change at run time: the board leaves the bus and comes back as the new device.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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

/* The host's rumble levels, 0..255 (large = left motor, small = right). Called from the USB task. */
typedef void (*usb_pad_rumble_cb_t)(uint8_t large, uint8_t small, void *arg);

/* audio: also be USB headphones (UAC2, 48 kHz 16-bit stereo, the host's volume and mute), a
 * composite device next to the gamepad; usb_pad_audio_read() returns what the host plays.
 * The Switch Pro mode is the gamepad alone. */
esp_err_t usb_pad_start(usb_pad_mode_t mode, usb_pad_rumble_cb_t rumble_cb, void *arg, bool audio);
/* Leave the bus and come back in another mode. Not from the USB task. */
esp_err_t usb_pad_set_mode(usb_pad_mode_t mode);
usb_pad_mode_t usb_pad_mode(void);
const char *usb_pad_mode_name(usb_pad_mode_t mode);
/* Queue a report; sent now if the IN endpoint is free, else as soon as it is. Thread-safe. */
void usb_pad_send(const usb_pad_state_t *state);
bool usb_pad_mounted(void);
/* Up to max_frames interleaved stereo frames the host played (volume applied), waiting up to
 * timeout_ms for the first. 0 while the host plays nothing. */
size_t usb_pad_audio_read(int16_t *pcm, size_t max_frames, uint32_t timeout_ms);
