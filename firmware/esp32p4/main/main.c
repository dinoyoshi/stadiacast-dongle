/*
 * Stadiacast dongle, step 1: bring up the ESP32-C6 hotspot and a DNS sink on the P4.
 *
 * What to expect on the serial console:
 *   - "hotspot up" with the SSID and the board's address
 *   - "station joined" with the controller's MAC when it connects
 *   - "dhcp" with the address it was given
 *   - "dns" lines with the Google names the controller asks for
 * The controller will then try HTTPS on port 443, which this step does not answer yet.
 */
#include "dongle.h"

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
    ESP_ERROR_CHECK(dns_sink_start(info.ip));

    ESP_LOGI(TAG, "ready: wake the controller (about 1 s press of the Stadia button)");
}
