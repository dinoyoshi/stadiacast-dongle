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
bool board_kv_get_i8(const char *ns, const char *key, int8_t *v);
bool board_kv_set_i8(const char *ns, const char *key, int8_t v);
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
/* The host has the USB device up (configured). */
bool board_usb_mounted(void);
/* How many gamepads the USB device is now (the players in the multi modes, else 1). */
int board_usb_players(void);
/* What the USB headset did, for the console. Counters run from power-up. Zeros without one. */
typedef struct {
    bool playing, recording;       /* the host streams to the headphones / from the microphone */
    bool mute[2];                  /* headphones, microphone */
    int16_t volume[2];             /* 1/256 dB */
    uint32_t packets, failed, overflows, missed; /* headphone packets: taken, failed, did not fit, lost */
    uint32_t mic_packets, mic_underruns, mic_skips, mic_dropped, mic_buffered;
} board_audio_stats_t;
void board_usb_audio_stats(board_audio_stats_t *st);

/* ---- the console's link to the host (on the P4: a USB serial port next to the gamepad) ---- */

#define BOARD_CONSOLE_LINE_MAX 160
/* The host has it open. */
bool board_console_active(void);
/* Queue bytes for the host: all of it or nothing (false). Any task. */
bool board_console_write(const char *s, size_t len);
/* Writes refused because the buffer was full. */
uint32_t board_console_dropped(void);
/* One line from the host, without its newline, into out (BOARD_CONSOLE_LINE_MAX bytes). */
bool board_console_read_line(char *out, uint32_t timeout_ms);
/* Raw mode for a firmware update: every byte the host sends goes to cb instead of becoming
 * lines; NULL goes back to lines. cb must not block. */
typedef void (*board_console_raw_cb_t)(const uint8_t *data, size_t n);
void board_console_set_raw(board_console_raw_cb_t cb);

/* ---- what the console shows about the board -------------------------------------------- */

typedef struct {
    const char *fw, *project, *date, *time, *sdk; /* firmware version, its build, the SDK's version */
    char chip[32];                                 /* e.g. "ESP32-P4 rev v1.3" */
    int cores;
    uint8_t mac[6];
    const char *ssid; /* the hotspot */
    int channel;
} board_info_t;
void board_info(board_info_t *out);

/* Once a second while the console is open (the board may poll slower things less often). */
typedef struct {
    int cpu[2];                 /* % busy per core since the last call, -1 unknown */
    unsigned heap[3];           /* free fast memory, its minimum ever, free bulk memory */
    bool rssi_ok;
    int rssi, stations;         /* the best signal among the stations on the hotspot, and how many */
} board_stats_t;
void board_stats(board_stats_t *out);
/* The console was closed: the next CPU figures and task list start fresh. */
void board_stats_reset(void);
/* {"t":"tasks",...} with each task's share of a core since the last call, into out; its length,
 * 0 for none (no earlier call to measure from, or no task list on this board). It may stall
 * interrupts (FreeRTOS's uxTaskGetSystemState): the console calls it only while the headphones
 * are idle, or when the host asks. */
int board_tasks_json(char *out, int cap);

/* Restart the board (the console's "reboot", and after a firmware update). */
void board_restart(void);

/* ---- the firmware's slots (on the P4: ota_0 and ota_1 with the bootloader's rollback) ---- */

typedef struct {
    const char *running, *next; /* slot names, "none" */
    size_t slot_size;           /* of the next one, 0 when there is none */
    bool trial;                 /* the running image is on trial (not yet kept) */
} board_fw_status_t;
void board_fw_status(board_fw_status_t *out);
/* Keep the running image (end its trial). False when it was not on trial or that failed. */
bool board_fw_keep(void);
/* The running image does not work: go back to the old one now (returns only if there is none). */
void board_fw_rollback(void);
/* Writing an image to the next slot: NULL when it worked, else what went wrong, for a person. */
const char *board_fw_begin(size_t size);
const char *board_fw_write(const void *data, size_t n);
/* All written: check it is an image for this board and this project, and boot it next.
 * version: the image's own (33 bytes). */
const char *board_fw_finish(char version[33]);
void board_fw_abort(void);
