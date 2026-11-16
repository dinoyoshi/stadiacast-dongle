/*
 * Rumble: the host's motor levels to the controller's motors, through the console's settings
 * (on/off and a strength per motor), plus a timed test the console starts.
 *
 * The host gives 0..255 per motor (large = left, small = right). Each goes out as
 * level * 257 * strength / 100, capped at 65535, so 100% is what the game asked for, below
 * that is gentler, and above that lifts weak rumble (a full-strength motor stays at full).
 * While a test runs it replaces the host's levels; when it ends the host's levels come back.
 * Each player's gamepad has its own levels; the settings are for all, the test is player 1's.
 */
#include "dongle_core.h"
#include "stadia.h"
#include "stadia_port.h"

static const char *TAG = "rumble";

static struct {
    port_mutex_t *lock;
    port_timer_t *test_timer;
    bool on;
    uint8_t pct[2];   /* strength per motor, RUMBLE_PCT_MAX at most */
    uint8_t host[USB_PAD_MAX_PLAYERS][2]; /* what the host asked for per player, 0..255 */
    uint8_t test[2];  /* a test's levels (player 1), 0..255 */
    bool testing;
    uint16_t out[USB_PAD_MAX_PLAYERS][2]; /* what went to each controller */
    volatile bool changed;
} R = {.on = true, .pct = {100, 100}};

static uint16_t scale(uint8_t level, uint8_t pct)
{
    uint32_t v = (uint32_t)level * 257u * pct / 100u;
    return v > 65535u ? 65535u : (uint16_t)v;
}

/* under the lock */
static void apply_player(int p)
{
    const uint8_t *lv = R.testing && p == 0 ? R.test : R.host[p];
    uint16_t l = R.on ? scale(lv[0], R.pct[0]) : 0;
    uint16_t r = R.on ? scale(lv[1], R.pct[1]) : 0;
    if (l == R.out[p][0] && r == R.out[p][1]) return;
    R.out[p][0] = l, R.out[p][1] = r;
    if (p == 0) R.changed = true;
    stadia_rumble(p, l, r);
}

static void apply(void)
{
    for (int p = 0; p < USB_PAD_MAX_PLAYERS; p++) apply_player(p);
}

static void test_end(void *arg)
{
    port_mutex_lock(R.lock);
    R.testing = false;
    apply_player(0);
    port_mutex_unlock(R.lock);
}

void rumble_init(void)
{
    R.lock = port_mutex_new();
    if (!R.lock) SLOGE(TAG, "no memory for the lock: no rumble");
    if (!(R.test_timer = port_timer_new(test_end, NULL, "rumble_test"))) SLOGW(TAG, "no timer: no rumble test");
}

void rumble_config(bool on, uint8_t left_pct, uint8_t right_pct)
{
    if (!R.lock) return;
    port_mutex_lock(R.lock);
    R.on = on;
    if (!on && R.testing) { /* off also ends a test */
        port_timer_stop(R.test_timer);
        R.testing = false;
    }
    R.pct[0] = left_pct > RUMBLE_PCT_MAX ? RUMBLE_PCT_MAX : left_pct;
    R.pct[1] = right_pct > RUMBLE_PCT_MAX ? RUMBLE_PCT_MAX : right_pct;
    apply(); /* a change shows at once, also on a motor that is running */
    port_mutex_unlock(R.lock);
}

void rumble_host(int player, uint8_t large, uint8_t small)
{
    if (!R.lock || player < 0 || player >= USB_PAD_MAX_PLAYERS) return;
    port_mutex_lock(R.lock);
    R.host[player][0] = large, R.host[player][1] = small;
    apply_player(player);
    port_mutex_unlock(R.lock);
}

const char *rumble_test(uint8_t left, uint8_t right, int ms)
{
    if (!R.lock || !R.test_timer) return "no rumble test in this firmware";
    stadia_stats_t s;
    stadia_get_stats(&s);
    if (!s.game_up) return "the controller is not connected";
    port_mutex_lock(R.lock);
    const char *err = NULL;
    if (!R.on) {
        err = "rumble is off";
    } else {
        port_timer_stop(R.test_timer); /* a new test replaces a running one */
        R.test[0] = left, R.test[1] = right;
        R.testing = left || right;
        if (R.testing) port_timer_start_once(R.test_timer, (int64_t)ms * 1000);
        apply_player(0);
    }
    port_mutex_unlock(R.lock);
    return err;
}

bool rumble_levels(uint16_t out[2], uint8_t host[2], bool *testing)
{
    if (!R.lock) return false;
    port_mutex_lock(R.lock);
    bool changed = R.changed;
    R.changed = false;
    out[0] = R.out[0][0], out[1] = R.out[0][1];
    host[0] = R.host[0][0], host[1] = R.host[0][1];
    *testing = R.testing;
    port_mutex_unlock(R.lock);
    return changed;
}
