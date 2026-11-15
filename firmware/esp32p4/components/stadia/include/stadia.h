/*
 * Stadiacast on the ESP32-P4: the servers a Stadia controller in Wi-Fi mode talks to.
 * A port of the Python receiver at the top of this repository (src/stadiacast/).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *advertise_ip; /* our address as the controller sees it, e.g. "192.168.4.1" (copied) */
    uint16_t https_port;      /* 443 */
    uint16_t dns_port;        /* 53; 0 = no DNS sink */
    uint16_t sntp_port;       /* 123; 0 = no SNTP server */
    uint16_t rtc_port;        /* 40000: the display link's UDP port (ICE/DTLS) */
    uint16_t game_port;       /* 40001: the game session's UDP port */
    bool start_game;          /* send StartGame once READY: the game session carries input and rumble */
    bool audio;               /* open the headset audio route at READY (SetAudioModeRequest mode 2) */
    uint8_t audio_complexity; /* Opus complexity 1..10; 0 = 8 */
    bool allow_clearcut;      /* false (the live preset): refuse Clearcut with a TLS alert */
    bool mic;                 /* decode what the controller sends as media (its microphone) */
    bool mic_request;         /* ask for it: SetAudioModeRequest with f2 set (a guess, see rtc.c) */
} stadia_config_t;

/* Start every server in its own task. False if one could not start. */
bool stadia_start(const stadia_config_t *cfg);

/*
 * The controller's gamepad state (src/stadiacast/gamepad.py). Bits 0 to 9 and 12 to 15 of
 * `buttons` match XInput's wButtons; 10 is Assistant, 11 Capture and 16 the Stadia button.
 * Sticks are -32768..32767 with Y positive = up, triggers 0..255.
 */
enum {
    STADIA_BTN_UP = 1 << 0,
    STADIA_BTN_DOWN = 1 << 1,
    STADIA_BTN_LEFT = 1 << 2,
    STADIA_BTN_RIGHT = 1 << 3,
    STADIA_BTN_MENU = 1 << 4,    /* XInput START */
    STADIA_BTN_OPTIONS = 1 << 5, /* XInput BACK */
    STADIA_BTN_L3 = 1 << 6,
    STADIA_BTN_R3 = 1 << 7,
    STADIA_BTN_L1 = 1 << 8,
    STADIA_BTN_R1 = 1 << 9,
    STADIA_BTN_ASSISTANT = 1 << 10,
    STADIA_BTN_CAPTURE = 1 << 11,
    STADIA_BTN_A = 1 << 12,
    STADIA_BTN_B = 1 << 13,
    STADIA_BTN_X = 1 << 14,
    STADIA_BTN_Y = 1 << 15,
    STADIA_BTN_STADIA = 1 << 16,
};

typedef struct {
    uint32_t buttons; /* STADIA_BTN_* */
    int16_t lx, ly, rx, ry;
    uint8_t lt, rt;
    bool connected; /* false: the controller went away (everything released) */
} stadia_pad_t;

/* Called from the link task on every input report (and with connected=false on a disconnect). */
typedef void (*stadia_pad_cb_t)(const stadia_pad_t *pad, void *arg);
void stadia_set_pad_callback(stadia_pad_cb_t cb, void *arg);

/* Set the rumble motors (0..65535 each), held until changed. Thread-safe. Needs the game
 * session: before it is up, the latest level is kept and sent once it connects. */
void stadia_rumble(uint16_t left, uint16_t right);

/* Headset audio: interleaved 48 kHz stereo PCM for the controller's headset jack, any number of
 * frames per call. Call it from one task, at the pace the audio plays: every 960 frames it
 * encodes one 20 ms Opus frame right there, which takes up to a few ms and STADIA_AUDIO_STACK
 * bytes of stack. Without a controller whose audio route is open it returns at once. */
#define STADIA_AUDIO_STACK (48 * 1024)
void stadia_audio_pcm(const int16_t *pcm, size_t frames);

/* The controller's microphone: mono 48 kHz PCM as it is decoded, from the microphone task, in
 * chunks of one Opus frame (usually 960 frames). The stream's own clock: the receiver buffers. */
typedef void (*stadia_mic_cb_t)(const int16_t *pcm, size_t frames, void *arg);
void stadia_set_mic_callback(stadia_mic_cb_t cb, void *arg);
