/* Run the Stadiacast core on Linux: stadia_host [ip] [https-port] [rtc-port]
 * STADIA_ALLOW_CLEARCUT=1 answers Clearcut instead of refusing it, as the fake controller expects. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "stadia.h"

int main(int argc, char **argv)
{
    stadia_config_t cfg = {
        .advertise_ip = argc > 1 ? argv[1] : "127.0.0.1",
        .https_port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 8443),
        .rtc_port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 40000),
        .allow_clearcut = getenv("STADIA_ALLOW_CLEARCUT") != NULL,
    };
    if (!stadia_start(&cfg)) {
        return 1;
    }
    fprintf(stderr, "stadia_host: ready\n");
    for (;;) {
        pause();
    }
}
