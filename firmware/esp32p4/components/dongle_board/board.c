/*
 * The FireBeetle 2 ESP32-P4's side of dongle_board.h: NVS for the key/value store, the LED on
 * CONFIG_DONGLE_STATUS_LED_GPIO, the BOOT button (GPIO35) and the USB device (usb_pad). The
 * console's link and figures are in board_console.c, the firmware slots in board_fw.c.
 */
#include "dongle_board.h"
#include "usb_pad.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#define BOOT_GPIO 35 /* low while pressed */

/* ---- NVS ------------------------------------------------------------------------------ */

bool board_kv_get_u8(const char *ns, const char *key, uint8_t *v)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_u8(h, key, v) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool board_kv_get_i8(const char *ns, const char *key, int8_t *v)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_i8(h, key, v) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool board_kv_get_str(const char *ns, const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool kv_write(const char *ns, const char *key, const uint8_t *u8, const int8_t *i8, const char *str)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = u8 ? nvs_set_u8(h, key, *u8) : i8 ? nvs_set_i8(h, key, *i8) : str ? nvs_set_str(h, key, str)
                                                                             : nvs_erase_key(h, key);
    if (nvs_commit(h) != ESP_OK) err = ESP_FAIL;
    nvs_close(h);
    return err == ESP_OK;
}

bool board_kv_set_u8(const char *ns, const char *key, uint8_t v)
{
    return kv_write(ns, key, &v, NULL, NULL);
}

bool board_kv_set_i8(const char *ns, const char *key, int8_t v)
{
    return kv_write(ns, key, NULL, &v, NULL);
}

bool board_kv_set_str(const char *ns, const char *key, const char *v)
{
    return kv_write(ns, key, NULL, NULL, v);
}

bool board_kv_erase(const char *ns, const char *key)
{
    return kv_write(ns, key, NULL, NULL, NULL);
}

/* ---- LED and button ------------------------------------------------------------------- */

bool board_led_init(void)
{
#if CONFIG_DONGLE_STATUS_LED_GPIO >= 0
    gpio_config_t io = {.pin_bit_mask = 1ULL << CONFIG_DONGLE_STATUS_LED_GPIO, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&io) == ESP_OK) return true;
    ESP_LOGW("led", "no status LED on GPIO%d", CONFIG_DONGLE_STATUS_LED_GPIO);
#endif
    return false;
}

void board_led(bool on)
{
#if CONFIG_DONGLE_STATUS_LED_GPIO >= 0
    gpio_set_level(CONFIG_DONGLE_STATUS_LED_GPIO, on);
#endif
}

bool board_button_init(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    return gpio_config(&io) == ESP_OK;
}

bool board_button_down(void)
{
    return gpio_get_level(BOOT_GPIO) == 0;
}

/* ---- USB ------------------------------------------------------------------------------ */

usb_pad_mode_t board_usb_mode(void)
{
    return usb_pad_mode();
}

bool board_usb_set_mode(usb_pad_mode_t mode)
{
    return usb_pad_set_mode(mode) == ESP_OK;
}

void board_usb_send(int player, const usb_pad_state_t *st)
{
    usb_pad_send(player, st);
}

bool board_usb_mounted(void)
{
    return usb_pad_mounted();
}

int board_usb_players(void)
{
    return usb_pad_players();
}

void board_usb_audio_stats(board_audio_stats_t *st)
{
    usb_pad_audio_stats_t u;
    usb_pad_audio_stats(&u);
    *st = (board_audio_stats_t){
        .playing = u.playing,
        .recording = u.recording,
        .mute = {u.mute[0], u.mute[1]},
        .volume = {u.volume[0], u.volume[1]},
        .packets = u.packets,
        .failed = u.failed,
        .overflows = u.overflows,
        .missed = u.missed,
        .mic_packets = u.mic_packets,
        .mic_underruns = u.mic_underruns,
        .mic_skips = u.mic_skips,
        .mic_dropped = u.mic_dropped,
        .mic_buffered = u.mic_buffered,
    };
}
