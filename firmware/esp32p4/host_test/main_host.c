/* Run the Stadiacast core on Linux: stadia_host [ip] [https-port] [rtc-port] [game-port]
 * STADIA_ALLOW_CLEARCUT=1 answers Clearcut instead of refusing it, as the fake controller expects.
 * STADIA_START_GAME=1 sends StartGame once READY (run the fake controller with --game).
 * Rumble test: pressing A starts both motors; 6 s after A is released they stop, so a held
 * level outlives the controller's 5 s haptics watchdog only through our refreshes. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "stadia.h"

static atomic_llong s_stop_at; /* ms, 0 = none */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void on_pad(const stadia_pad_t *pad, void *arg)
{
    static bool a_was;
    bool a = pad->connected && (pad->buttons & STADIA_BTN_A);
    if (a && !a_was) {
        atomic_store(&s_stop_at, 0);
        stadia_rumble(40000, 20000);
    } else if (!a && a_was) {
        atomic_store(&s_stop_at, now_ms() + 6000);
    }
    a_was = a;
}

int main(int argc, char **argv)
{
    uint16_t rtc_port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 40000);
    stadia_config_t cfg = {
        .advertise_ip = argc > 1 ? argv[1] : "127.0.0.1",
        .https_port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 8443),
        .rtc_port = rtc_port,
        .game_port = (uint16_t)(argc > 4 ? atoi(argv[4]) : rtc_port + 1),
        .start_game = getenv("STADIA_START_GAME") != NULL,
        .allow_clearcut = getenv("STADIA_ALLOW_CLEARCUT") != NULL,
    };
    stadia_set_pad_callback(on_pad, NULL);
    if (!stadia_start(&cfg)) {
        return 1;
    }
    fprintf(stderr, "stadia_host: ready\n");
    for (;;) {
        usleep(50 * 1000);
        long long stop = atomic_load(&s_stop_at);
        if (stop && now_ms() >= stop) {
            atomic_store(&s_stop_at, 0);
            stadia_rumble(0, 0);
        }
    }
}
