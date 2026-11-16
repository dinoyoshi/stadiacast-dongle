/*
 * Stadiacast dongle: the ESP32-C6 hotspot, then the servers the controller talks to.
 *
 * What to expect on the serial console after waking the controller:
 *   - "station joined" and "dhcp" when it connects to the hotspot
 *   - "dns" lines with the Google names it asks for
 *   - "https" lines for each request: generate_204, stadiaconfig, update2, the refused Clearcut
 *     upload, FirmwareVersionCheck, RegisterController, then /gmsg polls
 *   - "answer queued" and "GMsg #1 delivered": the controller has our WebRTC answer
 *   - "display: DTLS connected", "display state: ready", then "StartGame" and "game session
 *     connected": the controller is up and its input goes to the USB gamepad
 *   - "SetAudioMode answered" and "headset audio route open": what the computer plays on the
 *     "Stadiacast Dongle" headphones goes to the controller's headset jack
 *   - "media from the controller" and "microphone stream": the controller sends its microphone,
 *     which the computer records from the same headset's microphone
 * With several players (the console's "players" setting), each controller gets a player number
 * ("player 2 is controller ...", lines tagged display2 and game2) and its own USB gamepad in the
 * Xbox 360 and HID modes; the headset audio and the microphone stay with player 1.
 * The USB gamepad (an Xbox 360 controller to the host) and the USB headphones are on the
 * board's high-speed USB-C port; the other USB-C port stays the serial console and programming
 * port.
 */
#include <stdio.h>

#include "dongle.h"
#include "dongle_board_esp32p4.h"
#include "stadia.h"
#include "usb_pad.h"

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "dongle";

#ifndef CONFIG_DONGLE_OPUS_COMPLEXITY
#define CONFIG_DONGLE_OPUS_COMPLEXITY 8
#endif
#ifndef CONFIG_DONGLE_OPUS_MIC_COMPLEXITY
#define CONFIG_DONGLE_OPUS_MIC_COMPLEXITY 4
#endif
#ifndef CONFIG_DONGLE_OPUS_BITRATE
#define CONFIG_DONGLE_OPUS_BITRATE 96
#endif

/* What this build offers, and the console's settings before anyone changes them (Kconfig). */
static const dongle_build_t s_build = {
#if CONFIG_DONGLE_USB_AUDIO
    .audio = true,
#endif
#if CONFIG_DONGLE_USB_MIC
    .mic_available = true,
#endif
    .defaults =
        {
            .gain_db = 0,
            .complexity = CONFIG_DONGLE_OPUS_COMPLEXITY,
            .mic_complexity = CONFIG_DONGLE_OPUS_MIC_COMPLEXITY,
            .kbps = CONFIG_DONGLE_OPUS_BITRATE,
#if CONFIG_DONGLE_USB_MIC
            .mic = true,
#endif
#if CONFIG_DONGLE_MIC_PUSH_TO_TALK
            .ptt = true,
#endif
            .rumble = true,
            .rumble_pct = {100, 100},
        },
    .repo_path = "firmware/esp32p4/console",
};

static const dongle_pair_t s_pairing = {pair_init, pair_start, pair_stop, pair_get_status};

/* Which players' controllers are connected, for the LED: steady while any is. */
static volatile uint32_t s_connected;

static void on_pad(int player, const stadia_pad_t *pad, void *arg)
{
    usb_pad_state_t st;
    dongle_pad_state(pad, &st); /* core/dongle/input.c */
    if (pad->connected) {
        s_connected |= 1u << player;
    } else {
        s_connected &= ~(1u << player);
    }
    status_led_set(s_connected ? LED_CONNECTED : LED_WAITING);
    if (pad->connected && player == 0) pad_mode_filter(&st); /* the mode combos are player 1's */
    usb_pad_send(player, &st);
    console_pad(player, &st);
}

/* The game's rumble (0..255 per motor) for a player's gamepad to that controller's motors,
 * through the console's rumble settings (rumble.c). */
static void on_rumble(int player, uint8_t large, uint8_t small, void *arg)
{
    rumble_host(player, large, small);
}

#if CONFIG_DONGLE_USB_AUDIO
/* The USB headphones' samples to the controller's headset jack: 10 ms at a time, encoded into
 * Opus by stadia_audio_pcm() every 20 ms (hence the stack). */
static void audio_task(void *arg)
{
    static int16_t pcm[480 * 2];
    for (;;) {
        size_t n = usb_pad_audio_read(pcm, 480, 100);
        if (!n) continue;
        int32_t g = console_gain_q15(); /* headroom before the encoder (the console's gain) */
        if (g != 32768) {
            for (size_t i = 0; i < n * 2; i++) pcm[i] = (int16_t)((pcm[i] * g) >> 15);
        }
        stadia_audio_pcm(pcm, n);
    }
}
#endif

#if CONFIG_DONGLE_USB_MIC
/* The controller's microphone, decoded, to the USB microphone. */
static void on_mic(const int16_t *pcm, size_t frames, void *arg)
{
    usb_pad_mic_write(pcm, frames);
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    status_led_start();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_DONGLE_USB_AUDIO
    bool audio = true;
#else
    bool audio = false;
#endif
    dongle_settings_t set;
    rumble_init();
    settings_load(&s_build, &set);
    const char *known[STADIA_MAX_CONTROLLERS];
    int players = players_load(known);
#if CONFIG_DONGLE_CONSOLE
    usb_pad_enable_console();
#endif
    if (usb_pad_start(pad_mode_load(CONFIG_DONGLE_PAD_MODE), players, on_rumble, NULL, audio) != ESP_OK) {
        ESP_LOGE(TAG, "no USB gamepad, see above");
    }
    stadia_set_pad_callback(on_pad, NULL);
    stadia_set_controller_callback(players_on_controller, NULL);
#if CONFIG_DONGLE_USB_MIC
    stadia_set_mic_callback(on_mic, NULL);
#endif
    pad_mode_start();

    c6_update_if_needed();
    esp_netif_t *ap = hotspot_start();

    esp_netif_ip_info_t info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap, &info));
    static char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    stadia_config_t cfg = {
        .advertise_ip = ip,
        .https_port = 443,
        .dns_port = 53,
        .sntp_port = 123,
        .rtc_port = 40000,
        .game_port = 40001,
        .start_game = true,
        .audio = audio,
        .audio_complexity = set.complexity,
        .audio_mic_complexity = set.mic_complexity,
        .audio_kbps = set.kbps,
        .mic = audio && set.mic,
        .mic_push_to_talk = set.ptt, /* the console's settings, CONFIG_DONGLE_* until it changes them */
        .max_controllers = (uint8_t)players,
    };
    for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) cfg.known_ids[i] = known[i];
    bool servers = stadia_start(&cfg);
    if (!servers) {
        ESP_LOGE(TAG, "a server did not start, see above");
    }

#if CONFIG_DONGLE_USB_AUDIO
    if (xTaskCreate(audio_task, "audio", STADIA_AUDIO_STACK, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the audio task: no headset audio");
    }
#endif
#if CONFIG_DONGLE_CONSOLE
    pair_setup(&esp32p4_ble, CONFIG_DONGLE_SSID, CONFIG_DONGLE_PASSWORD);
    console_set_pairing(&s_pairing);
    console_start(cfg.mic, players);
    board_console_start();
#endif
    ota_confirm_when_up(servers);
    status_led_set(LED_WAITING);
    ESP_LOGI(TAG, "ready for %d controller%s: wake the controller (about 1 s press of the Stadia button)", players,
             players > 1 ? "s" : "");
}
