/*
 * The gamepad on the P4's high-speed USB-C port, in one of several modes the host knows without
 * a driver:
 *   Xbox 360    wired Xbox 360 controller (XInput): Windows, Linux, SteamOS, Android
 *   DualShock 4 a PS4 controller over USB: PC games and Steam (not a real PS4, which checks
 *               for Sony hardware)
 *   Switch Pro  a Nintendo Switch Pro Controller over USB: the Switch, Steam, Linux
 *   HID         a plain HID gamepad: macOS, Android, older DirectInput PC games
 * The mode can change at run time: the board leaves the bus and comes back as the new device.
 *
 * Several players: in the Xbox 360 and HID modes the board can be up to USB_PAD_MAX_PLAYERS
 * gamepads at once (one interface each, player 1 first), one per Stadia controller. The
 * DualShock 4 and Switch Pro modes are one gamepad (hosts know those by their USB ID).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "gamepad.h"

/* The host's rumble levels for one player's gamepad (0 = player 1), 0..255 (large = left motor,
 * small = right). Called from the USB task. */
typedef void (*usb_pad_rumble_cb_t)(int player, uint8_t large, uint8_t small, void *arg);

/* audio: also be a USB headset (UAC2: 48 kHz 16-bit stereo headphones and a mono microphone, each
 * with the host's volume and mute), a composite device next to the gamepad;
 * usb_pad_audio_read() returns what the host plays, usb_pad_mic_write() feeds the microphone.
 * The Switch Pro mode is the gamepad alone. */
esp_err_t usb_pad_start(usb_pad_mode_t mode, int players, usb_pad_rumble_cb_t rumble_cb, void *arg, bool audio);
/* How many gamepads the board is in its current mode: the players asked for in the Xbox 360 and
 * HID modes, else 1. */
int usb_pad_players(void);
/* Whether a mode can be several gamepads. */
bool usb_pad_mode_multi(usb_pad_mode_t mode);
/* Before usb_pad_start(): also be a USB serial port, the console (usb_pad_console_*). */
void usb_pad_enable_console(void);
/* Leave the bus and come back in another mode. Not from the USB task. */
esp_err_t usb_pad_set_mode(usb_pad_mode_t mode);
usb_pad_mode_t usb_pad_mode(void);
const char *usb_pad_mode_name(usb_pad_mode_t mode);
/* Queue a player's report; sent now if its IN endpoint is free, else as soon as it is. A player
 * the current mode has no gamepad for is dropped. Thread-safe. */
void usb_pad_send(int player, const usb_pad_state_t *state);
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
    uint32_t missed;               /* headphone packets lost (the endpoint was not armed in time);
                                    * each is filled in with a repeat of the next one */
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
/* Raw mode, for a firmware update (core/dongle/ota.c): while set, every byte the host sends goes to
 * cb, from the USB task, instead of becoming lines. NULL goes back to lines. cb must not block. */
typedef void (*usb_console_raw_cb_t)(const uint8_t *data, size_t n);
void usb_console_set_raw(usb_console_raw_cb_t cb);
