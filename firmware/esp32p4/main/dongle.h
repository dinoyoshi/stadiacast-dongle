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

/* Pairing (pair.c): give a controller in Bluetooth setup mode the hotspot's name and password
 * through the C6's Bluetooth, as the Stadia app did. Runs in its own task; the console starts it
 * and polls the status (console_set_pairing). */
bool pair_init(void);
const char *pair_start(void); /* NULL when started, else why not */
void pair_stop(void);
void pair_get_status(pair_status_t *out);
