/*
 * A wired Xbox 360 controller (XInput) on the P4's high-speed USB-C port, so Windows, Linux,
 * SteamOS and Android see a standard gamepad with rumble without any driver.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* XINPUT_GAMEPAD, as the 20-byte report carries it. */
typedef struct {
    uint16_t buttons; /* XInput wButtons */
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry; /* Y positive = up */
} usb_pad_state_t;

/* The host's rumble levels, 0..255 (large = left motor, small = right). Called from the USB task. */
typedef void (*usb_pad_rumble_cb_t)(uint8_t large, uint8_t small, void *arg);

esp_err_t usb_pad_start(usb_pad_rumble_cb_t rumble_cb, void *arg);
/* Queue a report; sent now if the IN endpoint is free, else as soon as it is. Thread-safe. */
void usb_pad_send(const usb_pad_state_t *state);
bool usb_pad_mounted(void);
