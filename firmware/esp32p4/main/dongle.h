#pragma once

#include "esp_netif.h"
#include "dongle_core.h" /* the portable part: core/dongle */
#include "stadia.h"
#include "usb_pad.h"

/* Start the C6 hotspot the controller joins; returns the access point's netif. */
esp_netif_t *hotspot_start(void);

/* Bring the C6's ESP-Hosted firmware to the version in the c6_fw partition, if there is one
 * there and the C6 runs another. Restarts the board after an update. */
void c6_update_if_needed(void);

/* The settings the console changes (console.c), kept in NVS. */
typedef struct {
    int8_t gain_db;     /* -12..0: the headphones' level before the Opus encoder */
    uint8_t complexity; /* Opus, 1..10 */
    uint8_t mic_complexity; /* Opus while the mic runs, 1..10 (applies when below complexity) */
    uint8_t kbps;       /* Opus bitrate, kbit/s (STADIA_AUDIO_KBPS_MIN..MAX) */
    bool mic;           /* ask the controller for its microphone (from the next start) */
    bool ptt;           /* push to talk */
    bool mic_running;   /* what this start does (mic as it was at power-up) */
    bool rumble;        /* rumble on/off */
    uint8_t rumble_pct[2]; /* strength per motor (left, right), 0..RUMBLE_PCT_MAX % */
} dongle_settings_t;
/* The saved settings (Kconfig's where none are saved). Before usb_pad_start and stadia_start. */
void settings_load(dongle_settings_t *out);
/* The headphones' gain as Q15 (32768 = 0 dB), for the audio task. */
int32_t console_gain_q15(void);
/* The console task, after stadia_start (CONFIG_DONGLE_CONSOLE). */
void console_start(bool mic_running, int players);
/* The input a player's gamepad gets, for the console's live view. Any task. */
void console_pad(int player, const usb_pad_state_t *st);

/* Pairing (pair.c): give a controller in Bluetooth setup mode the hotspot's name and password
 * through the C6's Bluetooth, as the Stadia app did. Runs in its own task; the console polls the
 * status. */
typedef struct {
    const char *state; /* idle, starting, scanning, connecting, reading, sending, waiting, done, failed */
    char msg[160];     /* what is going on, or why it failed, for a person */
    char name[32];     /* the controller found, "" before one */
    int rssi;
    int setup_state;   /* the controller's last setup state byte, -1 none */
    bool busy;
    uint32_t gen;      /* changes whenever anything above does */
} pair_status_t;
bool pair_init(void);
const char *pair_start(void); /* NULL when started, else why not */
void pair_stop(void);
void pair_get_status(pair_status_t *out);

/* Firmware updates over the console (ota.c). ota_run() handles the console's "ota" command: a
 * whole update, after which the board restarts (it returns only when the update failed).
 * send is the console's line printf. */
void ota_run(char *args, void (*send)(const char *fmt, ...));
/* {"t":"ota","state":"idle",...}: the slots, and whether this firmware is on trial. */
void ota_send_status(void (*send)(const char *fmt, ...));
/* A freshly updated image boots on trial: keep it once USB is up (else the bootloader rolls back
 * at the next restart). servers_up false (stadia_start failed) rolls back now. Call once at
 * start-up, after stadia_start. */
void ota_confirm_when_up(bool servers_up);
