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
 * The USB gamepad (an Xbox 360 controller to the host) and the USB headphones are on the
 * board's high-speed USB-C port; the other USB-C port stays the serial console and programming
 * port.
 */
#include <stdio.h>

#include "dongle.h"
#include "stadia.h"
#include "usb_pad.h"

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "dongle";

/* XInput wButtons share bits 0-9 and 12-15 with the controller's layout; the Stadia button is
 * the guide button (0x0400), Assistant and Capture have no XInput equivalent. */
#define XINPUT_SHARED_BITS 0xF3FF
#define XINPUT_GUIDE 0x0400

static void on_pad(const stadia_pad_t *pad, void *arg)
{
    usb_pad_state_t st = {0}; /* a disconnect releases everything */
    if (pad->connected) {
        st.buttons = (pad->buttons & XINPUT_SHARED_BITS) | ((pad->buttons & STADIA_BTN_STADIA) ? XINPUT_GUIDE : 0);
        st.lt = pad->lt, st.rt = pad->rt;
        st.lx = pad->lx, st.ly = pad->ly, st.rx = pad->rx, st.ry = pad->ry;
    }
    usb_pad_send(&st);
}

/* The game's rumble (0..255 per motor) to the controller's motors (0..65535). */
static void on_rumble(uint8_t large, uint8_t small, void *arg)
{
    stadia_rumble((uint16_t)(large * 257), (uint16_t)(small * 257));
}

#if CONFIG_DONGLE_USB_AUDIO
/* The USB headphones' samples to the controller's headset jack: 10 ms at a time, encoded into
 * Opus by stadia_audio_pcm() every 20 ms (hence the stack). */
static void audio_task(void *arg)
{
    static int16_t pcm[480 * 2];
    for (;;) {
        size_t n = usb_pad_audio_read(pcm, 480, 100);
        if (n) stadia_audio_pcm(pcm, n);
    }
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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_DONGLE_USB_AUDIO
    bool audio = true;
#else
    bool audio = false;
#endif
    if (usb_pad_start(on_rumble, NULL, audio) != ESP_OK) {
        ESP_LOGE(TAG, "no USB gamepad, see above");
    }
    stadia_set_pad_callback(on_pad, NULL);

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
#if CONFIG_DONGLE_USB_AUDIO
        .audio_complexity = CONFIG_DONGLE_OPUS_COMPLEXITY,
#endif
    };
    if (!stadia_start(&cfg)) {
        ESP_LOGE(TAG, "a server did not start, see above");
    }

#if CONFIG_DONGLE_USB_AUDIO
    xTaskCreate(audio_task, "audio", STADIA_AUDIO_STACK, NULL, 6, NULL);
#endif
    ESP_LOGI(TAG, "ready: wake the controller (about 1 s press of the Stadia button)");
}
