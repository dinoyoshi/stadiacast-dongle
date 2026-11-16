/*
 * Firmware updates over the console's USB serial port, for the web console's "Update firmware"
 * card: no ESP-IDF, no esptool, no second cable.
 *
 * The protocol is the same on every board; the slots are the board's (dongle_board.h). On the
 * ESP32-P4 the flash has two app slots (ota_0, ota_1 in partitions.csv). The update goes to the
 * one not running, and the board only switches to it once it is all there and checks out:
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
 * While the image comes in, the console is in raw mode (board_console_set_raw): the bytes go to a
 * stream buffer in PSRAM that is bigger than the window, so the USB task never has to wait.
 * Stats pause; the gamepad and the headset keep going, apart from short stalls while flash is
 * written.
 *
 * Rollback (on the P4: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a new image boots on trial. If the
 * controller's servers did not start (stadia_start), it goes straight back to the old one. Once
 * the host has opened the USB device and the board has been up for TRIAL_S seconds,
 * ota_confirm_task keeps it. If it crashes or is reset before that, the bootloader goes back to
 * the old one.
 */
#include "dongle_board.h"
#include "dongle_core.h"
#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/sha256.h"

static const char *TAG = "ota";

#define WINDOW (16 * 1024)       /* bytes the host may send ahead of the last "at" */
#define SB_SIZE (2 * WINDOW)     /* never full: the host stays within WINDOW */
#define WRITE_CHUNK 4096
#define IDLE_TIMEOUT_US (10 * 1000 * 1000)
#define TRIAL_S 10

static port_stream_t *s_sb;
static volatile uint32_t s_overflow;

static void raw_in(const uint8_t *data, size_t n)
{
    if (!port_stream_send(s_sb, data, n)) s_overflow++;
}

static void raw_discard(const uint8_t *data, size_t n)
{
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
    board_fw_status_t f;
    board_fw_status(&f);
    send("{\"t\":\"ota\",\"state\":\"idle\",\"can\":%s,\"running\":\"%s\",\"next\":\"%s\",\"slot\":%u,\"trial\":%s}",
         f.slot_size ? "true" : "false", f.running, f.next, (unsigned)f.slot_size, f.trial ? "true" : "false");
}

/* The whole update, in the console task: it returns when the update failed (the board restarts
 * when it worked). */
void ota_run(char *args, void (*send)(const char *fmt, ...))
{
#define FAIL(...)                                                                                             \
    do {                                                                                                      \
        char m_[96];                                                                                          \
        snprintf(m_, sizeof(m_), __VA_ARGS__);                                                                \
        SLOGW(TAG, "update failed: %s", m_);                                                               \
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
    bool begun = false, hashing = false, raw = false;
    uint8_t *buf = NULL;
    mbedtls_sha256_context sha;
    board_fw_status_t f;
    board_fw_status(&f);
    uint8_t want[32], got[32];
    bool ok;
    const char *err;

    if (!f.slot_size) FAIL("no second app slot: flash this once over the programming port (see the README)");
    long size = size_s ? strtol(size_s, NULL, 10) : 0;
    if (size <= 0 || !hash_s) FAIL("usage: ota begin <size> <sha256>");
    if ((size_t)size > f.slot_size) FAIL("the image (%ld bytes) is bigger than the slot (%u)", size, (unsigned)f.slot_size);
    hex_to_bin(hash_s, want, sizeof(want), &ok);
    if (!ok) FAIL("bad sha256");

    /* an update while the running image is still on trial: it runs the console, so keep it */
    if (f.trial) board_fw_keep();

    buf = malloc(WRITE_CHUNK);
    if (!s_sb) s_sb = port_stream_new(SB_SIZE, true);
    if (!buf || !s_sb) FAIL("no memory");
    port_stream_reset(s_sb);
    s_overflow = 0;

    SLOGI(TAG, "update: %ld bytes to %s, erasing", size, f.next);
    send("{\"t\":\"ota\",\"state\":\"erasing\"}");
    err = board_fw_begin((size_t)size);
    if (err) FAIL("%s", err);
    begun = true;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    hashing = true;

    board_console_set_raw(raw_in);
    raw = true;
    send("{\"t\":\"ota\",\"state\":\"ready\",\"win\":%d}", WINDOW);
    size_t at = 0, acked = 0;
    int64_t last = port_now_us();
    while (at < (size_t)size) {
        size_t want_n = (size_t)size - at;
        if (want_n > WRITE_CHUNK) want_n = WRITE_CHUNK;
        size_t n = port_stream_receive(s_sb, buf, want_n, 100);
        if (s_overflow) FAIL("the host sent too far ahead");
        if (!n) {
            if (port_now_us() - last > IDLE_TIMEOUT_US) FAIL("nothing from the host for 10 s");
            continue;
        }
        last = port_now_us();
        err = board_fw_write(buf, n);
        if (err) FAIL("%s", err);
        mbedtls_sha256_update(&sha, buf, n);
        at += n;
        if (at - acked >= WRITE_CHUNK || at == (size_t)size) {
            send("{\"t\":\"ota\",\"state\":\"recv\",\"at\":%u}", (unsigned)at);
            acked = at;
        }
    }
    board_console_set_raw(NULL);
    raw = false;

    mbedtls_sha256_finish(&sha, got);
    mbedtls_sha256_free(&sha);
    hashing = false;
    if (memcmp(got, want, sizeof(got))) FAIL("SHA-256 mismatch: the image was damaged on the way");

    send("{\"t\":\"ota\",\"state\":\"verifying\"}");
    char version[33];
    begun = false; /* finish ends the write either way */
    err = board_fw_finish(version);
    if (err) FAIL("%s", err);

    SLOGI(TAG, "update: %s written to %s, restarting into it", version, f.next);
    send("{\"t\":\"ota\",\"state\":\"done\",\"version\":\"%.32s\",\"slot\":\"%s\",\"reconnect\":true}", version,
         f.next);
    port_sleep_ms(300); /* let the answer leave before the board leaves the bus */
    board_restart();

out:
    if (raw) {
        /* what the host still had on the way is image, not commands */
        board_console_set_raw(raw_discard);
        port_sleep_ms(500);
    }
    board_console_set_raw(NULL);
    if (hashing) mbedtls_sha256_free(&sha);
    if (begun) board_fw_abort();
    free(buf);
#undef FAIL
}

/* A new image on trial: keep it once the host has the USB device up and TRIAL_S have passed. */
static void ota_confirm_task(void *arg)
{
    while (!(board_usb_mounted() && port_now_us() >= TRIAL_S * 1000000LL)) {
        port_sleep_ms(1000);
    }
    if (board_fw_keep()) {
        SLOGI(TAG, "the new firmware works: kept (no rollback)");
    }
}

void ota_confirm_when_up(bool servers_up)
{
    board_fw_status_t f;
    board_fw_status(&f);
    if (!f.trial) return;
    if (!servers_up) {
        /* boots, but the controller could never connect: no reason to keep it */
        SLOGE(TAG, "new firmware in %s on trial, but a server did not start: going back to the old one", f.running);
        port_sleep_ms(500); /* let the log out */
        board_fw_rollback();
        return; /* only when there is nothing to go back to */
    }
    SLOGI(TAG, "new firmware on trial in %s: kept once USB is up for %d s, rolled back if it restarts first", f.running,
          TRIAL_S);
    if (!port_spawn_prio(ota_confirm_task, NULL, "ota_ok", 3072, 1)) {
        board_fw_keep(); /* better than a rollback for want of memory */
    }
}
