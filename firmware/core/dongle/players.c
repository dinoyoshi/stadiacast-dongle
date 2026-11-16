/*
 * Players: how many Stadia controllers the dongle serves at once, and which controller is which
 * player. Player 1 is the one with the headset audio and microphone, and the first gamepad.
 *
 * The board's store, namespace "players": "count" (u8, 1..STADIA_MAX_CONTROLLERS) and "id0".."id3", the device id of the
 * controller that had each player number. The Stadia core reports a new owner through
 * players_on_controller(); the next start hands the ids back so each controller gets its number
 * again (a new controller takes a free number, or the one heard from the longest ago).
 */
#include "dongle_board.h"
#include "dongle_core.h"
#include "stadia_port.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "players";

static char s_ids[STADIA_MAX_CONTROLLERS][STADIA_DEVICE_ID_MAX];

int players_load(const char *ids[STADIA_MAX_CONTROLLERS])
{
    int count = 1;
    uint8_t v;
    if (board_kv_get_u8("players", "count", &v) && v >= 1 && v <= STADIA_MAX_CONTROLLERS) count = v;
    for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) {
        char key[4] = {'i', 'd', (char)('0' + i), 0};
        if (!board_kv_get_str("players", key, s_ids[i], sizeof(s_ids[i]))) s_ids[i][0] = '\0';
    }
    for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) ids[i] = s_ids[i];
    return count;
}

void players_save_count(int n)
{
    if (n < 1 || n > STADIA_MAX_CONTROLLERS) return;
    board_kv_set_u8("players", "count", (uint8_t)n);
}

void players_on_controller(int player, const char *device_id, void *arg)
{
    if (player < 0 || player >= STADIA_MAX_CONTROLLERS) return;
    char key[4] = {'i', 'd', (char)('0' + player), 0};
    if (device_id && device_id[0]) {
        board_kv_set_str("players", key, device_id);
        SLOGI(TAG, "player %d is controller %s from now on", player + 1, device_id);
    } else {
        board_kv_erase("players", key);
    }
}
