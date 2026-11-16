/*
 * Firmware updates over the console's USB serial port, for the web console's "Update firmware"
 * card: no ESP-IDF, no esptool, no second cable.
 *
 * The flash has two app slots (ota_0, ota_1 in partitions.csv). The update goes to the one not
 * running, and the board only switches to it once it is all there and checks out:
 *
 *   host: ota begin <size> <sha256 hex>
 *   board: erases the other slot, then {"t":"ota","state":"ready","win":16384}
 *   host: the image, as raw bytes, never more than win bytes past the last "at"
 *   board: {"t":"ota","state":"recv","at":<bytes written>} every few KB
 *   board: after the last byte: checks the SHA-256, that ESP-IDF accepts the image (chip,
 *          revision, its own hash) and that it is a Stadiacast dongle build, sets it to boot
 *          next, {"t":"ota","state":"done","version":...}, and restarts
 *   anything wrong: {"t":"ota","state":"failed","msg":...}, and the running firmware stays
 *
 * While the image comes in, the console is in raw mode (usb_console_set_raw): the bytes go to a
 * stream buffer in PSRAM that is bigger than the window, so the USB task never has to wait.
 * Stats pause; the gamepad and the headset keep going, apart from short stalls while flash is
 * written.
 *
 * Rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a new image boots on trial. If the
 * controller's servers did not start (stadia_start), it goes straight back to the old one. Once
 * the host has opened the USB device and the board has been up for TRIAL_S seconds,
 * ota_confirm_task keeps it. If it crashes or is reset before that, the bootloader goes back to
 * the old one.
 */
#include "dongle.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota";

#define WINDOW (16 * 1024)       /* bytes the host may send ahead of the last "at" */
#define SB_SIZE (2 * WINDOW)     /* never full: the host stays within WINDOW */
#define WRITE_CHUNK 4096
#define IDLE_TIMEOUT_US (10 * 1000 * 1000)
#define TRIAL_S 10

static StreamBufferHandle_t s_sb;
static volatile uint32_t s_overflow;

static void raw_in(const uint8_t *data, size_t n)
{
    if (xStreamBufferSend(s_sb, data, n, 0) != n) s_overflow++;
}

static void raw_discard(const uint8_t *data, size_t n)
{
}

static const char *slot_name(const esp_partition_t *p)
{
    return p ? p->label : "none";
}

static void hex_to_bin(const char *hex, uint8_t *out, size_t n, bool *ok)
{
    *ok = strlen(hex) == n * 2;
    for (size_t i = 0; *ok && i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) *ok = false;
        out[i] = (uint8_t)v;
    }
}

void ota_send_status(void (*send)(const char *fmt, ...))
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    send("{\"t\":\"ota\",\"state\":\"idle\",\"can\":%s,\"running\":\"%s\",\"next\":\"%s\",\"slot\":%u,\"trial\":%s}",
         next ? "true" : "false", slot_name(run), slot_name(next), next ? (unsigned)next->size : 0,
         st == ESP_OTA_IMG_PENDING_VERIFY ? "true" : "false");
}

/* The whole update, in the console task: it returns when the update failed (the board restarts
 * when it worked). */
void ota_run(char *args, void (*send)(const char *fmt, ...))
{
#define FAIL(...)                                                                                             \
    do {                                                                                                      \
        char m_[96];                                                                                          \
        snprintf(m_, sizeof(m_), __VA_ARGS__);                                                                \
        ESP_LOGW(TAG, "update failed: %s", m_);                                                               \
        send("{\"t\":\"ota\",\"state\":\"failed\",\"msg\":\"%s\"}", m_);                                       \
        goto out;                                                                                             \
    } while (0)

    char *save = NULL;
    char *sub = strtok_r(args, " \t", &save);
    if (!sub || strcmp(sub, "begin")) {
        ota_send_status(send);
        return;
    }
    char *size_s = strtok_r(NULL, " \t", &save);
    char *hash_s = strtok_r(NULL, " \t", &save);
    esp_ota_handle_t h = 0;
    bool begun = false, hashing = false, raw = false;
    uint8_t *buf = NULL;
    mbedtls_sha256_context sha;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    uint8_t want[32], got[32];
    bool ok;

    if (!part) FAIL("no second app slot: flash this once over the programming port (see the README)");
    long size = size_s ? strtol(size_s, NULL, 10) : 0;
    if (size <= 0 || !hash_s) FAIL("usage: ota begin <size> <sha256>");
    if ((size_t)size > part->size) FAIL("the image (%ld bytes) is bigger than the slot (%u)", size, (unsigned)part->size);
    hex_to_bin(hash_s, want, sizeof(want), &ok);
    if (!ok) FAIL("bad sha256");

    /* an update while the running image is still on trial: it runs the console, so keep it */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t run_st;
    if (run && esp_ota_get_state_partition(run, &run_st) == ESP_OK && run_st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    buf = malloc(WRITE_CHUNK);
    if (!s_sb) s_sb = xStreamBufferCreateWithCaps(SB_SIZE, 1, MALLOC_CAP_SPIRAM);
    if (!buf || !s_sb) FAIL("no memory");
    xStreamBufferReset(s_sb);
    s_overflow = 0;

    ESP_LOGI(TAG, "update: %ld bytes to %s, erasing", size, part->label);
    send("{\"t\":\"ota\",\"state\":\"erasing\"}");
    esp_err_t err = esp_ota_begin(part, (size_t)size, &h);
    if (err != ESP_OK) FAIL("esp_ota_begin: %s", esp_err_to_name(err));
    begun = true;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    hashing = true;

    usb_console_set_raw(raw_in);
    raw = true;
    send("{\"t\":\"ota\",\"state\":\"ready\",\"win\":%d}", WINDOW);
    size_t at = 0, acked = 0;
    int64_t last = esp_timer_get_time();
    while (at < (size_t)size) {
        size_t want_n = (size_t)size - at;
        if (want_n > WRITE_CHUNK) want_n = WRITE_CHUNK;
        size_t n = xStreamBufferReceive(s_sb, buf, want_n, pdMS_TO_TICKS(100));
        if (s_overflow) FAIL("the host sent too far ahead");
        if (!n) {
            if (esp_timer_get_time() - last > IDLE_TIMEOUT_US) FAIL("nothing from the host for 10 s");
            continue;
        }
        last = esp_timer_get_time();
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) FAIL("esp_ota_write: %s", esp_err_to_name(err));
        mbedtls_sha256_update(&sha, buf, n);
        at += n;
        if (at - acked >= WRITE_CHUNK || at == (size_t)size) {
            send("{\"t\":\"ota\",\"state\":\"recv\",\"at\":%u}", (unsigned)at);
            acked = at;
        }
    }
    usb_console_set_raw(NULL);
    raw = false;

    mbedtls_sha256_finish(&sha, got);
    mbedtls_sha256_free(&sha);
    hashing = false;
    if (memcmp(got, want, sizeof(got))) FAIL("SHA-256 mismatch: the image was damaged on the way");

    send("{\"t\":\"ota\",\"state\":\"verifying\"}");
    err = esp_ota_end(h);
    begun = false;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) FAIL("not a valid image for this chip");
    if (err != ESP_OK) FAIL("esp_ota_end: %s", esp_err_to_name(err));

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK) FAIL("no app description in the image");
    const esp_app_desc_t *self = esp_app_get_description();
    if (strncmp(desc.project_name, self->project_name, sizeof(desc.project_name)))
        FAIL("not a Stadiacast dongle image (it is %.32s)", desc.project_name);

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) FAIL("esp_ota_set_boot_partition: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "update: %s written to %s, restarting into it", desc.version, part->label);
    send("{\"t\":\"ota\",\"state\":\"done\",\"version\":\"%.32s\",\"slot\":\"%s\",\"reconnect\":true}", desc.version,
         part->label);
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the answer leave before the board leaves the bus */
    esp_restart();

out:
    if (raw) {
        /* what the host still had on the way is image, not commands */
        usb_console_set_raw(raw_discard);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    usb_console_set_raw(NULL);
    if (hashing) mbedtls_sha256_free(&sha);
    if (begun) esp_ota_abort(h);
    free(buf);
#undef FAIL
}

/* A new image on trial: keep it once the host has the USB device up and TRIAL_S have passed. */
static void ota_confirm_task(void *arg)
{
    while (!(usb_pad_mounted() && esp_timer_get_time() >= TRIAL_S * 1000000LL)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "the new firmware works: kept (no rollback)");
    }
    vTaskDelete(NULL);
}

void ota_confirm_when_up(bool servers_up)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    if (!servers_up) {
        /* boots, but the controller could never connect: no reason to keep it */
        ESP_LOGE(TAG, "new firmware in %s on trial, but a server did not start: going back to the old one",
                 run->label);
        vTaskDelay(pdMS_TO_TICKS(500)); /* let the log out */
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return; /* only when there is nothing to go back to */
    }
    ESP_LOGI(TAG, "new firmware on trial in %s: kept once USB is up for %d s, rolled back if it restarts first",
             run->label, TRIAL_S);
    if (xTaskCreate(ota_confirm_task, "ota_ok", 3072, NULL, 1, NULL) != pdPASS) {
        esp_ota_mark_app_valid_cancel_rollback(); /* better than a rollback for want of memory */
    }
}
