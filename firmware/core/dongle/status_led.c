/*
 * The board's LED (board_led(); GPIO3 on the FireBeetle 2 ESP32-P4) shows what the dongle is doing, so it
 * can be used without the serial console:
 *   fast blink   starting up (or updating the C6), the hotspot is not up yet
 *   slow blink   ready: wake the controller
 *   steady on    the controller is connected
 *   n blinks     the gamepad mode just picked (1 Xbox 360, 2 DualShock 4, 3 Switch Pro, 4 HID)
 */
#include "dongle_board.h"
#include "dongle_core.h"
#include "stadia_port.h"

#define TICK_US (100 * 1000)

static volatile led_state_t s_state = LED_STARTING;
static uint32_t s_tick;
static volatile int s_flash; /* ticks left of a status_led_flash() pattern */
static volatile int s_flash_len;

static void tick(void *arg)
{
    s_tick++;
    int on;
    if (s_flash > 0) { /* n blinks of 200 ms on, 200 ms off, then 600 ms dark */
        int t = s_flash_len - s_flash--;
        on = t < s_flash_len - 6 && (t % 4) < 2;
        board_led(on);
        return;
    }
    switch (s_state) {
    case LED_STARTING: on = s_tick & 1; break;          /* 5 Hz */
    case LED_WAITING: on = (s_tick % 10) < 5; break;    /* 1 Hz */
    default: on = 1;
    }
    board_led(on);
}

void status_led_start(void)
{
    if (!board_led_init()) return; /* the board has none */
    port_timer_t *t = port_timer_new(tick, NULL, "led");
    if (!t || !port_timer_start_periodic(t, TICK_US)) SLOGW("led", "no timer: the status LED stays as it is");
}

void status_led_flash(int n)
{
    s_flash_len = n * 4 + 6;
    s_flash = s_flash_len;
}

void status_led_set(led_state_t state)
{
    s_state = state;
}
