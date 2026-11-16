/*
 * Picking the gamepad mode from the controller: hold the Stadia button with a face button for
 * 1.5 s (a 4 s hold of the Stadia button turns the controller off). The board leaves USB and
 * comes back as the new kind of controller, remembers it (NVS) for the next power-up, and
 * pulses the controller's motors once per mode number:
 *   Stadia + A   Xbox 360      (1 pulse)
 *   Stadia + B   DualShock 4   (2 pulses)
 *   Stadia + X   Switch Pro    (3 pulses)
 *   Stadia + Y   HID gamepad   (4 pulses)
 * While the Stadia button is held with a face button, neither goes to the host.
 * A press of the board's BOOT button (GPIO35) steps to the next mode, controller or not. The
 * board's LED blinks the mode number either way.
 */
#include "dongle.h"
#include "stadia.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "pad_mode";

#define HOLD_US (1500 * 1000)
#define TICK_US (100 * 1000)
#define FACE (USB_PAD_A | USB_PAD_B | USB_PAD_X | USB_PAD_Y)
#define BOOT_GPIO 35 /* low while pressed */

/* The controller reports only changes, so a steady hold is timed by a 100 ms tick. */
static struct {
    volatile usb_pad_mode_t want;
    volatile int64_t since;   /* 0: no combo held */
    volatile bool latched;    /* fired: wait for the buttons to be let go */
    volatile bool busy;
    esp_timer_handle_t tick;
    int boot_low; /* ticks the BOOT button has been down */
} M;

usb_pad_mode_t pad_mode_load(void)
{
    usb_pad_mode_t mode = CONFIG_DONGLE_PAD_MODE;
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("pad", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "mode", &v) == ESP_OK && v < USB_PAD_MODE_COUNT) mode = v;
        nvs_close(h);
    }
    return mode;
}

static void save(usb_pad_mode_t mode)
{
    nvs_handle_t h;
    if (nvs_open("pad", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "mode", (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

static void switch_task(void *arg)
{
    usb_pad_mode_t mode = (usb_pad_mode_t)(intptr_t)arg;
    ESP_LOGI(TAG, "switching to %s", usb_pad_mode_name(mode));
    if (usb_pad_set_mode(mode) == ESP_OK) save(mode);
    status_led_flash((int)usb_pad_mode() + 1);
    for (int i = 0; i <= (int)usb_pad_mode(); i++) {
        stadia_rumble(40000, 40000);
        vTaskDelay(pdMS_TO_TICKS(200));
        stadia_rumble(0, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    M.busy = false;
    vTaskDelete(NULL);
}

static void start_switch(usb_pad_mode_t mode)
{
    M.busy = true;
    if (xTaskCreate(switch_task, "pad_mode", 4096, (void *)(intptr_t)mode, 5, NULL) != pdPASS) M.busy = false;
}

bool pad_mode_request(usb_pad_mode_t mode)
{
    if (mode >= USB_PAD_MODE_COUNT || M.busy) return false;
    if (mode == usb_pad_mode()) return true;
    start_switch(mode);
    return M.busy;
}

static void tick(void *arg)
{
    /* BOOT: a press (two ticks low) steps to the next mode */
    if (gpio_get_level(BOOT_GPIO) == 0) {
        if (++M.boot_low == 2 && !M.busy) {
            start_switch((usb_pad_mode_t)((usb_pad_mode() + 1) % USB_PAD_MODE_COUNT));
        }
    } else {
        M.boot_low = 0;
    }
    int64_t since = M.since;
    if (!since || M.latched || M.busy || esp_timer_get_time() - since < HOLD_US) return;
    M.latched = true;
    start_switch(M.want);
}

void pad_mode_start(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    const esp_timer_create_args_t args = {.callback = tick, .name = "pad_mode"};
    if (esp_timer_create(&args, &M.tick) != ESP_OK || esp_timer_start_periodic(M.tick, TICK_US) != ESP_OK) {
        ESP_LOGE(TAG, "no timer: the mode combos and the BOOT button won't work");
    }
}

void pad_mode_filter(usb_pad_state_t *st)
{
    uint16_t face = st->buttons & FACE;
    if (!(st->buttons & USB_PAD_GUIDE) || !face) {
        M.since = 0;
        M.latched = false;
        return;
    }
    st->buttons &= (uint16_t)~(USB_PAD_GUIDE | FACE); /* the combo is ours */
    usb_pad_mode_t want = face == USB_PAD_A   ? USB_PAD_XINPUT
                          : face == USB_PAD_B ? USB_PAD_DS4
                          : face == USB_PAD_X ? USB_PAD_SWITCH
                          : face == USB_PAD_Y ? USB_PAD_HID
                                              : USB_PAD_MODE_COUNT;
    if (want == USB_PAD_MODE_COUNT) { /* two face buttons: not a combo */
        M.since = 0;
        return;
    }
    if (!M.since || want != M.want) {
        M.want = want;
        M.since = esp_timer_get_time();
    }
}
