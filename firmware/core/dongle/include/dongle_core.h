/*
 * The dongle's own logic, the same on every board (core/dongle): turning the controller's input
 * into a gamepad state, the mode combos, rumble with the console's settings, which controller is
 * which player, and the status LED's patterns. The board provides dongle_board.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gamepad.h"
#include "stadia.h"

/* The controller's input as the USB gamepad's state (input.c): XInput's wButtons bits are the
 * controller's own (bits 0-9, 12-15), the Stadia button is the guide button, Assistant and
 * Capture go in extra. A disconnected pad is all released. */
void dongle_pad_state(const stadia_pad_t *pad, usb_pad_state_t *st);

/* The board's LED: fast blink while starting, slow blink when ready, steady on with the
 * controller connected. */
typedef enum { LED_STARTING, LED_WAITING, LED_CONNECTED } led_state_t;
void status_led_start(void);
void status_led_set(led_state_t state);
/* n blinks, then back to the state's pattern. */
void status_led_flash(int n);

/* The gamepad mode: the one saved last time (or default_mode), and the Stadia + face button
 * combos and the board's button that change it (pad_mode.c). pad_mode_filter() takes the combo
 * out of a report. */
usb_pad_mode_t pad_mode_load(usb_pad_mode_t default_mode);
/* The 100 ms tick that times the combos and watches the board's button (next mode). */
void pad_mode_start(void);
void pad_mode_filter(usb_pad_state_t *st);
/* Change the gamepad mode as the combos do (saved, the board leaves the bus and comes back).
 * False while a change is under way. */
bool pad_mode_request(usb_pad_mode_t mode);

/* Players (players.c): how many controllers the dongle serves at once (1..STADIA_MAX_CONTROLLERS,
 * from the next start) and which controller each player number belongs to, kept in the board's
 * store ("players") so a controller keeps its number across restarts. */
int players_load(const char *ids[STADIA_MAX_CONTROLLERS]); /* the count; ids point to static strings */
void players_save_count(int n);
/* stadia_controller_cb_t: a player number went to another controller (or was freed). */
void players_on_controller(int player, const char *device_id, void *arg);

/* Rumble (rumble.c): the host's levels to the controller's motors through the console's
 * settings, and the console's test. rumble_init() before the settings are applied. */
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

/* ---- settings (settings.c), kept in the board's store ("console") ------------------------ */

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

/* What this build offers, and the settings before anyone changes them. */
typedef struct {
    bool audio;         /* the USB headset is built in */
    bool mic_available; /* the controller's microphone can be passed on */
    dongle_settings_t defaults;
    const char *repo_path; /* where the web console lives, for the log: "firmware/esp32p4/console" */
} dongle_build_t;

/* The saved settings (the defaults where none are saved). Before the USB device and
 * stadia_start: it also applies the gain and the rumble settings. */
void settings_load(const dongle_build_t *build, dongle_settings_t *out);
/* The headphones' gain as Q15 (32768 = 0 dB), for the audio task. */
int32_t console_gain_q15(void);

/* ---- the console (console.c): JSON lines over the board's console link ------------------- */

/* Pairing a controller over the board's Bluetooth (pair.c, on the board's dongle_ble_t), which
 * the console starts and polls. A board without it passes none. */
typedef struct {
    const char *state; /* idle, starting, scanning, connecting, reading, sending, waiting, done, failed */
    char msg[160];     /* what is going on, or why it failed, for a person */
    char name[32];     /* the controller found, "" before one */
    int rssi;
    int setup_state;   /* the controller's last setup state byte, -1 none */
    bool busy;
    uint32_t gen;      /* changes whenever anything above does */
} pair_status_t;
typedef struct {
    bool (*init)(void);
    const char *(*start)(void); /* NULL when started, else why not */
    void (*stop)(void);
    void (*get_status)(pair_status_t *out);
} dongle_pair_t;
/* Before console_start; NULL (the default): no pairing on this board. */
void console_set_pairing(const dongle_pair_t *pair);

/* ---- pairing (pair.c): the Stadia app's Wi-Fi setup over Bluetooth LE -------------------- */

/* A Bluetooth LE device address: its type (public, random) and 6 bytes, least significant first. */
typedef struct {
    uint8_t type;
    uint8_t val[6];
} dongle_ble_addr_t;

/* What the board's Bluetooth reports, from its own task (keep it short, never block). */
typedef struct {
    /* An advertisement heard while scanning: its first 16-bit service data UUID (0: none) and
     * its name (not NUL-terminated, len 0: none). */
    void (*adv)(const dongle_ble_addr_t *addr, int rssi, uint16_t svc_data_uuid16, const char *name,
                size_t name_len);
    void (*scan_done)(void); /* the scan ended by itself */
    void (*disconnected)(int reason);
    void (*notify)(uint16_t handle, const uint8_t *data, size_t len);
} dongle_ble_events_t;

/* A board's Bluetooth LE central, one link at a time. Every call but cancel comes from the
 * pairing task; the blocking ones return 0 when done, else an error (the board's own codes). */
typedef struct {
    /* Bring Bluetooth up, the first time (it then stays up); NULL when up, else why not. */
    const char *(*start)(const dongle_ble_events_t *events);
    int (*scan_start)(int ms); /* active scan, every advertisement reported */
    void (*scan_stop)(void);
    /* Connect: 0 connected, DONGLE_BLE_CONNECT_FAILED when it could not even start */
    int (*connect)(const dongle_ble_addr_t *addr, int timeout_ms);
    /* Exchange the MTU (the result is ignored), and the MTU now in use (23 by default). */
    void (*exchange_mtu)(int timeout_ms);
    unsigned (*mtu)(void);
    /* Find the characteristics with these 128-bit UUIDs (little endian, as on the air): each
     * one's value handle, 0 when the device has none. */
    int (*find_chrs)(const uint8_t (*uuids)[16], int n, uint16_t *handles, int timeout_ms);
    /* Write (a long write when it does not fit the MTU) and wait for the answer. */
    int (*write)(uint16_t handle, const void *data, size_t len, int timeout_ms);
    /* A long read, appended to buf at *len (what does not fit is dropped). */
    int (*read_long)(uint16_t handle, uint8_t *buf, size_t cap, size_t *len, int timeout_ms);
    /* End the link, and return once it is down (or after about a second). */
    void (*disconnect)(void);
    /* From any task (pair_stop): stop a scan, drop the link, end the call that waits. */
    void (*cancel)(void);
} dongle_ble_t;
#define DONGLE_BLE_CONNECT_FAILED (-1)

/* The board's Bluetooth and the hotspot pairing gives the controller: before pair_init. Without
 * it pair_init fails (the console then says the board has no pairing). */
void pair_setup(const dongle_ble_t *ble, const char *ssid, const char *password);
/* The dongle_pair_t a board passes to console_set_pairing. */
bool pair_init(void);
const char *pair_start(void); /* NULL when started, else why not */
void pair_stop(void);
void pair_get_status(pair_status_t *out);

/* The console task, after stadia_start. */
void console_start(bool mic_running, int players);
/* The input a player's gamepad gets, for the console's live view. Any task. */
void console_pad(int player, const usb_pad_state_t *st);
/* Whether the host asked for the log ("logs on"): the board's log hook then copies each line
 * to the console link (not starting with '{'). */
bool console_logs_on(void);

/* ---- firmware updates over the console (ota.c) ------------------------------------------ */

/* The console's "ota" command: a whole update, after which the board restarts (it returns only
 * when the update failed). send is the console's line printf. */
void ota_run(char *args, void (*send)(const char *fmt, ...));
/* {"t":"ota","state":"idle",...}: the slots, and whether this firmware is on trial. */
void ota_send_status(void (*send)(const char *fmt, ...));
/* A freshly updated image boots on trial: keep it once USB is up (else the board goes back at
 * the next restart). servers_up false (stadia_start failed) goes back now. Once at start-up,
 * after stadia_start. */
void ota_confirm_when_up(bool servers_up);
