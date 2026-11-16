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

/* Bluetooth LE for pairing (ble_central.c): the C6's radio and NimBLE, for core/dongle/pair.c. */
extern const dongle_ble_t esp32p4_ble;
