/*
 * Stadiacast on the ESP32-P4: the servers a Stadia controller in Wi-Fi mode talks to.
 * A port of the Python receiver at the top of this repository (src/stadiacast/).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *advertise_ip; /* our address as the controller sees it, e.g. "192.168.4.1" */
    uint16_t https_port;      /* 443 */
    uint16_t dns_port;        /* 53; 0 = no DNS sink */
    uint16_t sntp_port;       /* 123; 0 = no SNTP server */
    uint16_t rtc_port;        /* 40000: the display link's UDP port (ICE/DTLS) */
    bool allow_clearcut;      /* false (the live preset): refuse Clearcut with a TLS alert */
} stadia_config_t;

/* Start every server in its own task. False if one could not start. */
bool stadia_start(const stadia_config_t *cfg);
