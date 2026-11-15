/*
 * The board's LED (GPIO3 on the FireBeetle 2 ESP32-P4) shows what the dongle is doing, so it
 * can be used without the serial console:
 *   fast blink   starting up (or updating the C6), the hotspot is not up yet
 *   slow blink   ready: wake the controller
 *   steady on    the controller is connected
 *   n blinks     the gamepad mode just picked (1 Xbox 360, 2 DualShock 4, 3 Switch Pro, 4 HID)
 */
#include "dongle.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

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
        gpio_set_level(CONFIG_DONGLE_STATUS_LED_GPIO, on);
        return;
    }
    switch (s_state) {
    case LED_STARTING: on = s_tick & 1; break;          /* 5 Hz */
    case LED_WAITING: on = (s_tick % 10) < 5; break;    /* 1 Hz */
    default: on = 1;
    }
    gpio_set_level(CONFIG_DONGLE_STATUS_LED_GPIO, on);
}

void status_led_start(void)
{
#if CONFIG_DONGLE_STATUS_LED_GPIO >= 0
    gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_DONGLE_STATUS_LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = tick, .name = "led"};
    if (gpio_config(&io) != ESP_OK || esp_timer_create(&args, &t) != ESP_OK || esp_timer_start_periodic(t, TICK_US) != ESP_OK) {
        ESP_LOGW("led", "no status LED on GPIO%d", CONFIG_DONGLE_STATUS_LED_GPIO);
    }
#endif
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
