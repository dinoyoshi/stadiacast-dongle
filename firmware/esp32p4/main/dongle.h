#pragma once

#include "esp_netif.h"
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
