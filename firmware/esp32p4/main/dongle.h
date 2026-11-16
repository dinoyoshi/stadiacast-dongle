#pragma once

#include "esp_netif.h"
#include "stadia.h"
#include "usb_pad.h"

/* Start the C6 hotspot the controller joins; returns the access point's netif. */
esp_netif_t *hotspot_start(void);

/* Bring the C6's ESP-Hosted firmware to the version in the c6_fw partition, if there is one
 * there and the C6 runs another. Restarts the board after an update. */
void c6_update_if_needed(void);

/* The board's LED: fast blink while starting, slow blink when ready, steady on with the
 * controller connected (CONFIG_DONGLE_STATUS_LED_GPIO, -1 for none). */
typedef enum { LED_STARTING, LED_WAITING, LED_CONNECTED } led_state_t;
void status_led_start(void);
void status_led_set(led_state_t state);
/* n blinks, then back to the state's pattern. */
void status_led_flash(int n);

/* The gamepad mode: the one saved last time (or CONFIG_DONGLE_PAD_MODE), and the Stadia + face
 * button combos and the BOOT button that change it (pad_mode.c). pad_mode_filter() takes the combo out of a report. */
usb_pad_mode_t pad_mode_load(void);
/* The 100 ms tick that times the combos and watches the board's BOOT button (next mode). */
void pad_mode_start(void);
void pad_mode_filter(usb_pad_state_t *st);

/* The settings the console changes (console.c), kept in NVS. */
typedef struct {
    int8_t gain_db;     /* -12..0: the headphones' level before the Opus encoder */
    uint8_t complexity; /* Opus, 1..10 */
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
/* Change the gamepad mode as the combos do (saved, the board leaves the bus and comes back).
 * False while a change is under way. */
bool pad_mode_request(usb_pad_mode_t mode);

/* Players (players.c): how many controllers the dongle serves at once (1..STADIA_MAX_CONTROLLERS,
 * from the next start) and which controller each player number belongs to, kept in NVS
 * ("players") so a controller keeps its number across restarts. */
int players_load(const char *ids[STADIA_MAX_CONTROLLERS]); /* the count; ids point to static strings */
void players_save_count(int n);
/* stadia_controller_cb_t: a player number went to another controller (or was freed). */
void players_on_controller(int player, const char *device_id, void *arg);

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

/* Rumble (rumble.c): the host's levels to the controller's motors through the console's
 * settings, and the console's test. rumble_init() before settings_load(). */
#define RUMBLE_PCT_MAX 200
#define RUMBLE_TEST_MS_MAX 5000
void rumble_init(void);
void rumble_config(bool on, uint8_t left_pct, uint8_t right_pct);
/* A player's host levels, 0..255 (large = left motor). From the USB task. */
void rumble_host(int player, uint8_t large, uint8_t small);
/* Run both motors at host-scale levels (0..255, through the settings) for ms, then back to the
 * host's levels; 0, 0 stops a test. NULL when it started, else why not. */
const char *rumble_test(uint8_t left, uint8_t right, int ms);
/* What the motors get (0..65535), what the host asks for, and whether a test runs; true when
 * the motors' levels changed since the last call. */
bool rumble_levels(uint16_t out[2], uint8_t host[2], bool *testing);

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
