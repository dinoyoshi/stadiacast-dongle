/*
 * Keep the C6's ESP-Hosted firmware at the version in the P4's c6_fw partition.
 *
 * At boot: read the version of the image in c6_fw (its esp_app_desc_t) and ask the C6 which
 * version it runs. If they differ, stream the image to the C6 with the ESP-Hosted slave OTA
 * RPCs (begin, write, end, activate), then restart the P4 so both sides start together on the
 * new firmware. The C6 keeps its old firmware in its other OTA slot until the new one is
 * written and verified, so a failed update leaves it as it was.
 *
 * NVS remembers the version last tried: if the C6 still reports another version after an
 * update, the next boots do not try the same image again (no update loop).
 */
#include "dongle.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "c6_update";

#define CHUNK 1400

/* The image's total length (header, segments, checksum, hash) and its version string. */
static bool read_image(const esp_partition_t *part, size_t *len, char *version, size_t vcap)
{
    esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK || hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        return false;
    }
    size_t off = sizeof(hdr);
    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) return false;
        if (i == 0) {
            esp_app_desc_t desc;
            if (esp_partition_read(part, off + sizeof(seg), &desc, sizeof(desc)) != ESP_OK ||
                desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                return false;
            }
            strlcpy(version, desc.version, vcap);
        }
        off += sizeof(seg) + seg.data_len;
        if (off > part->size) return false;
    }
    off += (16 - (off % 16)) % 16; /* padding, then the checksum byte, padded again */
    off += 1;
    if (hdr.hash_appended) {
        off += (16 - (off % 16)) % 16;
        off += 32;
    }
    *len = off;
    return off <= part->size;
}

static bool already_tried(const char *version)
{
    nvs_handle_t h;
    char tried[32] = "";
    size_t n = sizeof(tried);
    if (nvs_open("c6", NVS_READONLY, &h) != ESP_OK) return false;
    bool same = nvs_get_str(h, "tried", tried, &n) == ESP_OK && strcmp(tried, version) == 0;
    nvs_close(h);
    return same;
}

static void remember_try(const char *version)
{
    nvs_handle_t h;
    if (nvs_open("c6", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "tried", version);
    nvs_commit(h);
    nvs_close(h);
}

static bool send_image(const esp_partition_t *part, size_t len)
{
    static uint8_t chunk[CHUNK];
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(err));
        return false;
    }
    int last_pct = -1;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        if (esp_partition_read(part, off, chunk, n) != ESP_OK ||
            (err = esp_hosted_slave_ota_write(chunk, (uint32_t)n)) != ESP_OK) {
            ESP_LOGE(TAG, "OTA write at %u: %s", (unsigned)off, esp_err_to_name(err));
            esp_hosted_slave_ota_end();
            return false;
        }
        int pct = (int)((off + n) * 100 / len);
        if (pct / 10 != last_pct / 10) ESP_LOGI(TAG, "%d%%", pct);
        last_pct = pct;
    }
    if ((err = esp_hosted_slave_ota_end()) != ESP_OK) {
        ESP_LOGE(TAG, "OTA end (the C6 rejected the image): %s", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_hosted_slave_ota_activate()) != ESP_OK) {
        ESP_LOGE(TAG, "OTA activate: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void c6_update_if_needed(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "c6_fw");
    size_t len = 0;
    char image[32] = "";
    if (!part || !read_image(part, &len, image, sizeof(image))) {
        ESP_LOGI(TAG, "no C6 firmware image on the board: the C6 keeps what it runs");
        return;
    }

    esp_hosted_init();
    esp_hosted_connect_to_slave();
    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) != ESP_OK) {
        ESP_LOGW(TAG, "could not read the C6's firmware version: not updating it");
        return;
    }
    char running[32];
    snprintf(running, sizeof(running), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, v.major1, v.minor1, v.patch1);
    if (strcmp(running, image) == 0) {
        ESP_LOGI(TAG, "C6 runs ESP-Hosted %s, as on the board", running);
        return;
    }
    if (already_tried(image)) {
        ESP_LOGW(TAG, "C6 runs %s, still not %s after an update: not trying again", running, image);
        return;
    }
    ESP_LOGW(TAG, "updating the C6 from ESP-Hosted %s to %s (%u bytes); keep the board powered", running, image,
             (unsigned)len);
    remember_try(image);
    if (!send_image(part, len)) {
        ESP_LOGE(TAG, "update failed: the C6 keeps %s", running);
        return;
    }
    ESP_LOGW(TAG, "C6 updated to %s: restarting so both sides start together", image);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
