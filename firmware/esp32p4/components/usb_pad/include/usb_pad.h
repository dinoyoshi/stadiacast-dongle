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

/* audio: also be a USB headset (UAC2: 48 kHz 16-bit stereo headphones and a mono microphone, each
 * with the host's volume and mute), a composite device next to the gamepad;
 * usb_pad_audio_read() returns what the host plays, usb_pad_mic_write() feeds the microphone.
 * The Switch Pro mode is the gamepad alone. */
esp_err_t usb_pad_start(usb_pad_mode_t mode, usb_pad_rumble_cb_t rumble_cb, void *arg, bool audio);
/* Before usb_pad_start(): also be a USB serial port, the console (usb_pad_console_*). */
void usb_pad_enable_console(void);
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

/* Mono 48 kHz samples for the microphone, from one task, at the pace they were recorded. They are
 * buffered (about 40 ms before the host gets the first) and the buffer follows the host's clock:
 * a backlog is trimmed, an underrun is filled with silence. Dropped while the board has no
 * microphone. */
void usb_pad_mic_write(const int16_t *pcm, size_t frames);

/* What the headset did, for the console. Counters run from power-up. */
typedef struct {
    bool playing, recording;       /* the host streams to the headphones / from the microphone */
    bool mute[2];                  /* headphones, microphone */
    int16_t volume[2];             /* 1/256 dB */
    uint32_t packets, failed;      /* headphone packets from the host */
    uint32_t overflows;            /* headphone packets that did not fit (the encoder fell behind) */
    uint32_t mic_packets, mic_underruns, mic_skips, mic_dropped;
    uint32_t mic_buffered;         /* samples waiting for the host */
} usb_pad_audio_stats_t;
void usb_pad_audio_stats(usb_pad_audio_stats_t *st);

/* ---- the console's USB serial port ------------------------------------------------------ */

#define USB_CONSOLE_LINE_MAX 160
/* The host has the port open (DTR, or it sent a line in the last few seconds). */
bool usb_console_active(void);
/* Queue bytes for the host, best whole lines: all of it or nothing (false: closed or full).
 * Any task. */
bool usb_console_write(const char *s, size_t len);
/* Writes refused because the buffer was full. */
uint32_t usb_console_dropped(void);
/* One line the host sent, without its newline, into out (USB_CONSOLE_LINE_MAX bytes). */
bool usb_console_read_line(char *out, uint32_t timeout_ms);
/* Raw mode, for a firmware update (main/ota.c): while set, every byte the host sends goes to
 * cb, from the USB task, instead of becoming lines. NULL goes back to lines. cb must not block. */
typedef void (*usb_console_raw_cb_t)(const uint8_t *data, size_t n);
void usb_console_set_raw(usb_console_raw_cb_t cb);
