/*
 * What the dongle's logic (core/dongle) needs from a board, apart from the OS (stadia_port.h).
 * The FireBeetle 2 ESP32-P4's are in esp32p4/main/board.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gamepad.h"

/* A small key/value store that survives restarts (NVS on an ESP32). Namespaces and keys are at
 * most 15 characters. Each set or erase is saved before it returns. False: not there (get), or
 * not saved. */
bool board_kv_get_u8(const char *ns, const char *key, uint8_t *v);
bool board_kv_set_u8(const char *ns, const char *key, uint8_t v);
bool board_kv_get_str(const char *ns, const char *key, char *out, size_t cap);
bool board_kv_set_str(const char *ns, const char *key, const char *v);
bool board_kv_erase(const char *ns, const char *key);

/* The status LED: false when the board has none (then nothing blinks). */
bool board_led_init(void);
void board_led(bool on);

/* A button on the board that steps to the next gamepad mode: false when there is none. */
bool board_button_init(void);
bool board_button_down(void);

/* The board's USB device: the gamepad mode it is now, a change of mode (it leaves the bus and
 * comes back; not from the USB task; false if it failed), and a player's report (thread-safe). */
usb_pad_mode_t board_usb_mode(void);
bool board_usb_set_mode(usb_pad_mode_t mode);
void board_usb_send(int player, const usb_pad_state_t *st);
