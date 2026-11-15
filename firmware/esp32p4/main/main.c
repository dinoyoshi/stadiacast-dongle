/*
 * Stadiacast dongle: the ESP32-C6 hotspot, then the servers the controller talks to.
 *
 * What to expect on the serial console after waking the controller:
 *   - "station joined" and "dhcp" when it connects to the hotspot
 *   - "dns" lines with the Google names it asks for
 *   - "https" lines for each request: generate_204, stadiaconfig, update2, the refused Clearcut
 *     upload, FirmwareVersionCheck, RegisterController, then /gmsg polls
 *   - "answer queued" and "GMsg #1 delivered": the controller has our WebRTC answer
 * Step 2b adds the UDP side (ICE, DTLS), so the controller then gives up and sleeps.
 */
#include <stdio.h>

#include "dongle.h"
#include "stadia.h"

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "dongle";

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
    };
    if (!stadia_start(&cfg)) {
        ESP_LOGE(TAG, "a server did not start, see above");
    }

    ESP_LOGI(TAG, "ready: wake the controller (about 1 s press of the Stadia button)");
}
