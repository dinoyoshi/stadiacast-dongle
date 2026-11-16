/*
 * core/dongle on a fake board: the input mapping, the mode combos and the board's button, rumble
 * through the settings and the console's test, players kept in the store, the stored mode, and
 * the console: commands and their JSON answers, the settings in the store, and a whole firmware
 * update (and a damaged one) over the console link. The fake board keeps its store in memory and
 * records what reaches the USB side, the controller's motors and its firmware slot; the Stadia
 * core's calls are faked too, so no servers run.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/sha256.h"

#include "dongle_board.h"
#include "dongle_core.h"
#include "stadia_port.h"

static int s_fail;
#define CHECK(c, ...)                                                                                                 \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            s_fail++;                                                                                                 \
        }                                                                                                             \
    } while (0)

/* ---- the fake board --------------------------------------------------------------------- */

static struct {
    char ns[16], key[16], val[64];
    bool is_u8;
} s_kv[32];
static int s_nkv;

static int kv_find(const char *ns, const char *key)
{
    for (int i = 0; i < s_nkv; i++) {
        if (!strcmp(s_kv[i].ns, ns) && !strcmp(s_kv[i].key, key)) return i;
    }
    return -1;
}
static int kv_slot(const char *ns, const char *key)
{
    int i = kv_find(ns, key);
    if (i < 0) {
        i = s_nkv++;
        snprintf(s_kv[i].ns, sizeof(s_kv[i].ns), "%s", ns);
        snprintf(s_kv[i].key, sizeof(s_kv[i].key), "%s", key);
    }
    return i;
}
bool board_kv_get_u8(const char *ns, const char *key, uint8_t *v)
{
    int i = kv_find(ns, key);
    if (i < 0 || !s_kv[i].is_u8) return false;
    *v = (uint8_t)s_kv[i].val[0];
    return true;
}
bool board_kv_set_u8(const char *ns, const char *key, uint8_t v)
{
    int i = kv_slot(ns, key);
    s_kv[i].val[0] = (char)v, s_kv[i].is_u8 = true;
    return true;
}
bool board_kv_get_i8(const char *ns, const char *key, int8_t *v)
{
    uint8_t u;
    if (!board_kv_get_u8(ns, key, &u)) return false;
    *v = (int8_t)u;
    return true;
}
bool board_kv_set_i8(const char *ns, const char *key, int8_t v)
{
    return board_kv_set_u8(ns, key, (uint8_t)v);
}
bool board_kv_get_str(const char *ns, const char *key, char *out, size_t cap)
{
    int i = kv_find(ns, key);
    if (i < 0 || s_kv[i].is_u8 || strlen(s_kv[i].val) + 1 > cap) return false;
    strcpy(out, s_kv[i].val);
    return true;
}
bool board_kv_set_str(const char *ns, const char *key, const char *v)
{
    int i = kv_slot(ns, key);
    snprintf(s_kv[i].val, sizeof(s_kv[i].val), "%s", v);
    s_kv[i].is_u8 = false;
    return true;
}
bool board_kv_erase(const char *ns, const char *key)
{
    int i = kv_find(ns, key);
    if (i >= 0) s_kv[i] = s_kv[--s_nkv];
    return true;
}

static atomic_int s_led_changes, s_led_on;
bool board_led_init(void)
{
    return true;
}
void board_led(bool on)
{
    if (on != s_led_on) s_led_changes++;
    s_led_on = on;
}

static atomic_bool s_button;
bool board_button_init(void)
{
    return true;
}
bool board_button_down(void)
{
    return s_button;
}

static atomic_int s_mode = USB_PAD_XINPUT, s_mode_changes;
usb_pad_mode_t board_usb_mode(void)
{
    return (usb_pad_mode_t)s_mode;
}
bool board_usb_set_mode(usb_pad_mode_t mode)
{
    s_mode = mode;
    s_mode_changes++;
    return true;
}
void board_usb_send(int player, const usb_pad_state_t *st)
{
}

bool board_usb_mounted(void)
{
    return true;
}
int board_usb_players(void)
{
    return 1;
}
void board_usb_audio_stats(board_audio_stats_t *st)
{
    memset(st, 0, sizeof(*st));
}

/* the console link: lines in from the test, lines out to a log the test searches */
static pthread_mutex_t s_io = PTHREAD_MUTEX_INITIALIZER;
static char s_in[8][BOARD_CONSOLE_LINE_MAX];
static int s_nin;
static char s_out[64 * 1024];
static size_t s_nout;
static board_console_raw_cb_t s_raw;
static atomic_int s_restarts;

bool board_console_active(void)
{
    return true;
}
bool board_console_write(const char *str, size_t len)
{
    pthread_mutex_lock(&s_io);
    if (s_nout + len < sizeof(s_out) - 1) {
        memcpy(s_out + s_nout, str, len);
        s_nout += len;
        s_out[s_nout] = 0;
    }
    pthread_mutex_unlock(&s_io);
    return true;
}
uint32_t board_console_dropped(void)
{
    return 0;
}
bool board_console_read_line(char *out, uint32_t timeout_ms)
{
    for (uint32_t t = 0; t <= timeout_ms; t += 5) {
        pthread_mutex_lock(&s_io);
        if (s_nin) {
            strcpy(out, s_in[0]);
            memmove(s_in[0], s_in[1], sizeof(s_in[0]) * (size_t)--s_nin);
            pthread_mutex_unlock(&s_io);
            return true;
        }
        pthread_mutex_unlock(&s_io);
        port_sleep_ms(5);
    }
    return false;
}
void board_console_set_raw(board_console_raw_cb_t cb)
{
    s_raw = cb;
}
void board_info(board_info_t *out)
{
    *out = (board_info_t){.fw = "test", .project = "stadiacast_dongle", .date = "Nov 16 2026", .time = "10:00:00",
                          .sdk = "host", .cores = 2, .mac = {1, 2, 3, 4, 5, 6}, .ssid = "stadiacast-dongle",
                          .channel = 6};
    strcpy(out->chip, "Linux host");
}
void board_stats(board_stats_t *out)
{
    *out = (board_stats_t){.cpu = {5, 7}, .heap = {1000, 900, 5000}, .rssi_ok = true, .rssi = -40, .stations = 1};
}
void board_stats_reset(void)
{
}
int board_tasks_json(char *out, int cap)
{
    return snprintf(out, cap, "{\"t\":\"tasks\",\"ms\":1000,\"list\":[]}\n");
}
void board_restart(void)
{
    s_restarts++;
}

/* the firmware slot */
static uint8_t s_slot[256 * 1024];
static size_t s_slot_len;
static bool s_slot_booting;
void board_fw_status(board_fw_status_t *out)
{
    *out = (board_fw_status_t){.running = "ota_0", .next = "ota_1", .slot_size = sizeof(s_slot)};
}
bool board_fw_keep(void)
{
    return false;
}
void board_fw_rollback(void)
{
}
const char *board_fw_begin(size_t size)
{
    s_slot_len = 0;
    s_slot_booting = false;
    return NULL;
}
const char *board_fw_write(const void *data, size_t n)
{
    if (s_slot_len + n > sizeof(s_slot)) return "too big";
    memcpy(s_slot + s_slot_len, data, n);
    s_slot_len += n;
    return NULL;
}
const char *board_fw_finish(char version[33])
{
    s_slot_booting = true;
    strcpy(version, "v-test");
    return NULL;
}
void board_fw_abort(void)
{
    s_slot_len = 0;
}

/* the core's side */
static atomic_int s_motor[STADIA_MAX_CONTROLLERS][2], s_pulses;
void stadia_rumble(int player, uint16_t left, uint16_t right)
{
    if (player == 0 && left && !s_motor[0][0]) s_pulses++;
    s_motor[player][0] = left;
    s_motor[player][1] = right;
}
static bool s_game_up = true;
void stadia_get_stats(stadia_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    st->game_up = s_game_up;
    st->display_state = "ready";
    st->battery_pct = -1;
    st->ping_us[0] = st->ping_us[1] = -1;
}
int stadia_get_players(stadia_player_t *out, int max)
{
    memset(out, 0, sizeof(*out));
    out->known = true;
    strcpy(out->device_id, "ctl-one");
    out->battery_pct = 80;
    out->ping_us = -1;
    return 1;
}
static int s_complexity, s_kbps;
static bool s_ptt;
void stadia_audio_set_complexity(int c)
{
    s_complexity = c;
}
void stadia_audio_set_mic_complexity(int c)
{
}
void stadia_audio_set_bitrate(int kbps)
{
    s_kbps = kbps;
}
void stadia_mic_set_push_to_talk(bool on)
{
    s_ptt = on;
}
bool stadia_mic_push_to_talk(void)
{
    return s_ptt;
}
void stadia_forget_player(int player)
{
}

/* ---- tests ------------------------------------------------------------------------------ */

static void test_input(void)
{
    stadia_pad_t pad = {.buttons = STADIA_BTN_A | STADIA_BTN_UP | STADIA_BTN_STADIA | STADIA_BTN_ASSISTANT |
                                   STADIA_BTN_CAPTURE | STADIA_BTN_MENU,
                        .lx = -32768, .ly = 32767, .rx = 5, .ry = -6, .lt = 7, .rt = 255, .connected = true};
    usb_pad_state_t st;
    dongle_pad_state(&pad, &st);
    CHECK(st.buttons == (USB_PAD_A | USB_PAD_UP | USB_PAD_GUIDE | USB_PAD_START), "buttons 0x%04x", st.buttons);
    CHECK(st.extra == (USB_PAD_CAPTURE | USB_PAD_ASSISTANT), "extra 0x%02x", st.extra);
    CHECK(st.lx == -32768 && st.ly == 32767 && st.rx == 5 && st.ry == -6 && st.lt == 7 && st.rt == 255, "axes");
    pad.connected = false;
    dongle_pad_state(&pad, &st);
    CHECK(!st.buttons && !st.extra && !st.lx && !st.rt, "a disconnect releases everything");
}

static void hold(uint16_t buttons, int ms)
{
    for (int t = 0; t < ms; t += 50) { /* the controller sends changes only, but sending more is fine */
        usb_pad_state_t st = {.buttons = buttons, .lx = 100};
        pad_mode_filter(&st);
        if (buttons & USB_PAD_GUIDE && buttons & (USB_PAD_A | USB_PAD_B | USB_PAD_X | USB_PAD_Y)) {
            int faces = __builtin_popcount(buttons & (USB_PAD_A | USB_PAD_B | USB_PAD_X | USB_PAD_Y));
            if (faces == 1) CHECK(st.buttons == 0 && st.lx == 100, "the combo stays off the host");
        }
        port_sleep_ms(50);
    }
}

static void test_modes(void)
{
    CHECK(pad_mode_load(USB_PAD_HID) == USB_PAD_HID, "default mode with nothing stored");
    s_pulses = 0;
    pad_mode_start();
    hold(USB_PAD_GUIDE | USB_PAD_B, 1000); /* too short */
    CHECK(s_mode_changes == 0, "no change before 1.5 s");
    hold(USB_PAD_GUIDE | USB_PAD_B, 800);
    hold(0, 100);
    CHECK(s_mode == USB_PAD_DS4, "Stadia + B held 1.8 s: DualShock 4 (now %d)", (int)s_mode);
    port_sleep_ms(1200); /* the pulses */
    CHECK(s_pulses == 2, "two pulses for mode 2 (got %d)", (int)s_pulses);
    CHECK(pad_mode_load(USB_PAD_XINPUT) == USB_PAD_DS4, "the mode is stored");
    hold(USB_PAD_GUIDE | USB_PAD_A | USB_PAD_B, 2000);
    CHECK(s_mode == USB_PAD_DS4, "two face buttons are no combo");

    s_button = true; /* the board's button: the next mode */
    port_sleep_ms(400);
    s_button = false;
    port_sleep_ms(1800);
    CHECK(s_mode == USB_PAD_SWITCH, "the button steps to the next mode (now %d)", (int)s_mode);
    CHECK(pad_mode_request(USB_PAD_HID), "console request taken");
    port_sleep_ms(2500);
    CHECK(s_mode == USB_PAD_HID, "console request");
    CHECK(s_led_changes > 0, "the LED blinked the mode");
}

static void test_rumble(void)
{
    rumble_init();
    rumble_config(true, 100, 100);
    rumble_host(1, 255, 128);
    CHECK(s_motor[1][0] == 65535 && s_motor[1][1] == 32896, "100%%: %d %d", s_motor[1][0], s_motor[1][1]);
    rumble_config(true, 50, 200);
    CHECK(s_motor[1][0] == 32767 && s_motor[1][1] == 65535, "50%% / 200%%: %d %d", s_motor[1][0], s_motor[1][1]);
    rumble_config(false, 50, 200);
    CHECK(s_motor[1][0] == 0 && s_motor[1][1] == 0, "off");
    rumble_config(true, 100, 100);
    CHECK(rumble_test(255, 255, 300) == NULL, "test starts");
    CHECK(s_motor[0][0] == 65535, "test runs");
    uint16_t out[2];
    uint8_t host[2];
    bool testing;
    CHECK(rumble_levels(out, host, &testing) && testing && out[0] == 65535, "levels show the test");
    port_sleep_ms(500);
    CHECK(s_motor[0][0] == 0 && rumble_levels(out, host, &testing) && !testing, "test ends after 300 ms");
    s_game_up = false;
    CHECK(rumble_test(255, 255, 300) != NULL, "no test without a controller");
}

static void test_players(void)
{
    const char *ids[STADIA_MAX_CONTROLLERS];
    CHECK(players_load(ids) == 1 && !ids[0][0], "nothing stored: one player, no ids");
    players_save_count(3);
    players_save_count(9); /* ignored */
    players_on_controller(1, "ctl-two", NULL);
    players_on_controller(0, "ctl-one", NULL);
    players_on_controller(0, "", NULL); /* freed */
    CHECK(players_load(ids) == 3, "count kept");
    CHECK(!ids[0][0] && !strcmp(ids[1], "ctl-two") && !ids[2][0], "ids kept: '%s' '%s'", ids[0], ids[1]);
}

/* ---- the console ------------------------------------------------------------------------- */

static void say(const char *line)
{
    pthread_mutex_lock(&s_io);
    snprintf(s_in[s_nin++], BOARD_CONSOLE_LINE_MAX, "%s", line);
    pthread_mutex_unlock(&s_io);
}

/* wait up to ms for text to show up in the output since mark */
static bool heard(size_t mark, const char *text, int ms)
{
    for (int t = 0; t <= ms; t += 10) {
        pthread_mutex_lock(&s_io);
        bool got = strstr(s_out + mark, text) != NULL;
        pthread_mutex_unlock(&s_io);
        if (got) return true;
        port_sleep_ms(10);
    }
    return false;
}

static size_t mark(void)
{
    pthread_mutex_lock(&s_io);
    size_t m = s_nout;
    pthread_mutex_unlock(&s_io);
    return m;
}

static const dongle_build_t s_build = {
    .audio = true,
    .mic_available = true,
    .defaults = {.complexity = 8, .mic_complexity = 4, .kbps = 96, .mic = true, .ptt = true, .rumble = true,
                 .rumble_pct = {100, 100}},
    .repo_path = "firmware/esp32p4/console",
};

static void send_image(const uint8_t *img, size_t n, const char *hash_hex)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "ota begin %zu %s", n, hash_hex);
    size_t m = mark();
    say(cmd);
    CHECK(heard(m, "\"state\":\"ready\",\"win\":16384", 2000), "ota ready");
    /* as the page does: never more than the window past the last "at" */
    size_t at = 0;
    for (int waited = 0; at < n && waited < 3000;) {
        size_t acked = 0; /* the last "at" the board sent */
        pthread_mutex_lock(&s_io);
        for (const char *p = s_out + m; (p = strstr(p, "\"at\":")); p += 5) acked = strtoul(p + 5, NULL, 10);
        pthread_mutex_unlock(&s_io);
        size_t chunk = n - at < 1000 ? n - at : 1000;
        if (at + chunk - acked > 16384) {
            port_sleep_ms(5);
            waited += 5;
            continue;
        }
        s_raw(img + at, chunk);
        at += chunk;
        waited = 0;
    }
}

static void test_console(void)
{
    dongle_settings_t set;
    settings_load(&s_build, &set);
    CHECK(set.complexity == 8 && set.kbps == 96 && set.ptt && set.gain_db == 0, "defaults");
    console_start(true, 1);

    size_t m = mark();
    say("hello");
    CHECK(heard(m, "{\"t\":\"info\",\"fw\":\"test\",\"project\":\"stadiacast_dongle\",\"built\":\"Nov 16 2026 10:00:00\","
                   "\"idf\":\"host\",\"chip\":\"Linux host\",\"cores\":2,\"mac\":\"01:02:03:04:05:06\","
                   "\"ssid\":\"stadiacast-dongle\",\"channel\":6,\"audio\":true,"
                   "\"modes\":[\"Xbox 360\",\"DualShock 4\",\"Switch Pro\",\"HID gamepad\"]}\n", 1000),
          "info line");
    CHECK(heard(m, "{\"t\":\"settings\",\"mode\":", 1000), "settings line");
    CHECK(heard(m, "{\"t\":\"ota\",\"state\":\"idle\",\"can\":true,\"running\":\"ota_0\",\"next\":\"ota_1\",\"slot\":262144,\"trial\":false}", 1000),
          "ota status line");
    CHECK(heard(m, "{\"t\":\"stats\",\"up\":", 1500), "stats once a second");
    CHECK(heard(m, "\"cpu\":[5,7],\"heap\":[1000,900,5000]", 10), "board figures in the stats");
    CHECK(heard(m, "\"players\":[{\"known\":true,\"id\":\"ctl-one\"", 10), "players in the stats");
    CHECK(heard(m, "{\"t\":\"tasks\"", 2500), "task list while the headphones are idle");

    m = mark();
    say("set gain -3");
    CHECK(heard(m, "{\"t\":\"ok\",\"key\":\"gain\"}", 1000), "set gain");
    CHECK(heard(m, "\"gain\":-3,", 1000), "settings show it");
    CHECK(console_gain_q15() == 23198, "gain -3 dB as Q15 (%d)", (int)console_gain_q15());
    int8_t g = 0;
    CHECK(board_kv_get_i8("console", "gain", &g) && g == -3, "gain stored");
    say("set bitrate 64");
    say("set complexity 5");
    say("set ptt 0");
    CHECK(heard(m, "{\"t\":\"ok\",\"key\":\"ptt\"}", 1000) && s_kbps == 64 && s_complexity == 5 && !s_ptt,
          "bitrate, complexity, ptt reach the core");
    m = mark();
    say("set bitrate 999");
    CHECK(heard(m, "{\"t\":\"err\",\"msg\":\"bitrate is 32-160 kbps\"}", 1000), "range checked");
    say("pair");
    CHECK(heard(m, "{\"t\":\"err\",\"msg\":\"this board has no pairing\"}", 1000), "no pairing here");
    say("frobnicate");
    CHECK(heard(m, "{\"t\":\"err\",\"msg\":\"unknown command\"}", 1000), "unknown command");
    s_game_up = true;
    say("rumble 255 0 200");
    CHECK(heard(m, "{\"t\":\"ok\",\"key\":\"rumble\"}", 1000), "rumble test");
    CHECK(heard(m, "{\"t\":\"rumble\",\"out\":[65535,0],\"host\":[", 1000), "motor levels shown");
    m = mark();
    say("defaults");
    CHECK(heard(m, "\"gain\":0,\"complexity\":8,\"mic_complexity\":4,\"bitrate\":96,", 1000) && s_kbps == 96,
          "defaults");
    dongle_settings_t again;
    settings_load(&s_build, &again);
    CHECK(again.gain_db == 0 && again.kbps == 96, "defaults stored");

    /* a firmware update: 100 KB through the console link */
    size_t n = 100 * 1024 + 123;
    uint8_t *img = malloc(n);
    for (size_t i = 0; i < n; i++) img[i] = (uint8_t)(i * 7 + (i >> 9));
    uint8_t h[32];
    mbedtls_sha256(img, n, h, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", h[i]);
    m = mark();
    send_image(img, n, hex);
    CHECK(heard(m, "{\"t\":\"ota\",\"state\":\"done\",\"version\":\"v-test\",\"slot\":\"ota_1\",\"reconnect\":true}", 3000),
          "update done");
    port_sleep_ms(500); /* the restart comes 300 ms after "done" */
    CHECK(s_slot_len == n && !memcmp(s_slot, img, n) && s_slot_booting && s_restarts == 1, "image in the slot, restart (%zu of %zu, booting %d, restarts %d)", s_slot_len, n, s_slot_booting, (int)s_restarts);

    /* a damaged one: the hash does not match, the slot is not booted */
    img[5000] ^= 1;
    m = mark();
    send_image(img, n, hex);
    CHECK(heard(m, "{\"t\":\"ota\",\"state\":\"failed\",\"msg\":\"SHA-256 mismatch: the image was damaged on the way\"}", 3000),
          "damaged image refused");
    CHECK(!s_slot_booting && s_restarts == 1, "not booted");
    port_sleep_ms(600);
    m = mark();
    say("get");
    CHECK(heard(m, "{\"t\":\"settings\"", 1000), "back to lines after the update");
    free(img);
}

int main(void)
{
    status_led_start();
    status_led_set(LED_WAITING);
    test_input();
    test_rumble();
    test_players();
    test_modes();
    test_console();
    if (s_fail) fprintf(stderr, "--- console output\n%s\n", s_out);
    printf("dongle logic: %s\n", s_fail ? "FAILED" : "ok");
    return s_fail ? 1 : 0;
}
