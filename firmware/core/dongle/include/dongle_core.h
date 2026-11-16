/*
 * The dongle's own logic, the same on every board (core/dongle): turning the controller's input
 * into a gamepad state, the mode combos, rumble with the console's settings, which controller is
 * which player, and the status LED's patterns. The board provides dongle_board.h.
 */
#pragma once

#include <stdbool.h>
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
