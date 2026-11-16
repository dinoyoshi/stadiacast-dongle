/*
 * core/dongle on a fake board: the input mapping, the mode combos and the board's button, rumble
 * through the settings and the console's test, players kept in the store, and the stored mode.
 * The fake board keeps its store in memory and records what reaches the USB side and the
 * controller's motors (stadia_rumble and stadia_get_stats are faked too, so no servers run).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    status_led_start();
    status_led_set(LED_WAITING);
    test_input();
    test_rumble();
    test_players();
    test_modes();
    printf("dongle logic: %s\n", s_fail ? "FAILED" : "ok");
    return s_fail ? 1 : 0;
}
