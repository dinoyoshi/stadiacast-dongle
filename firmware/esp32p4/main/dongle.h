#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/* Start the C6 hotspot the controller joins; returns the access point's netif. */
esp_netif_t *hotspot_start(void);

/* Answer every DNS question (A) with `ip`, the board's own address, and log each name asked.
 * AAAA gets an empty answer so the controller stays on IPv4, as in Stadiacast's dns_sink. */
esp_err_t dns_sink_start(esp_ip4_addr_t ip);
