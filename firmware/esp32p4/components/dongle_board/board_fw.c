/*
 * The FireBeetle 2 ESP32-P4's firmware slots for core/dongle/ota.c (dongle_board.h): ESP-IDF's
 * OTA partitions (ota_0, ota_1) with the bootloader's rollback.
 */
#include "dongle_board.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static esp_ota_handle_t s_h;
static const esp_partition_t *s_part;
static char s_err[96];

static const char *slot_name(const esp_partition_t *p)
{
    return p ? p->label : "none";
}

static const char *failed(const char *what, esp_err_t err)
{
    snprintf(s_err, sizeof(s_err), "%s: %s", what, esp_err_to_name(err));
    return s_err;
}

void board_fw_status(board_fw_status_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run && esp_ota_get_state_partition(run, &st) != ESP_OK) st = ESP_OTA_IMG_UNDEFINED;
    *out = (board_fw_status_t){
        .running = slot_name(run),
        .next = slot_name(next),
        .slot_size = next ? next->size : 0,
        .trial = st == ESP_OTA_IMG_PENDING_VERIFY,
    };
}

bool board_fw_keep(void)
{
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

void board_fw_rollback(void)
{
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

const char *board_fw_begin(size_t size)
{
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) return "no second app slot";
    esp_err_t err = esp_ota_begin(s_part, size, &s_h);
    return err == ESP_OK ? NULL : failed("esp_ota_begin", err);
}

const char *board_fw_write(const void *data, size_t n)
{
    esp_err_t err = esp_ota_write(s_h, data, n);
    return err == ESP_OK ? NULL : failed("esp_ota_write", err);
}

const char *board_fw_finish(char version[33])
{
    esp_err_t err = esp_ota_end(s_h);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) return "not a valid image for this chip";
    if (err != ESP_OK) return failed("esp_ota_end", err);

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(s_part, &desc) != ESP_OK) return "no app description in the image";
    const esp_app_desc_t *self = esp_app_get_description();
    if (strncmp(desc.project_name, self->project_name, sizeof(desc.project_name))) {
        snprintf(s_err, sizeof(s_err), "not a Stadiacast dongle image (it is %.32s)", desc.project_name);
        return s_err;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) return failed("esp_ota_set_boot_partition", err);
    snprintf(version, 33, "%.32s", desc.version);
    return NULL;
}

void board_fw_abort(void)
{
    esp_ota_abort(s_h);
}
