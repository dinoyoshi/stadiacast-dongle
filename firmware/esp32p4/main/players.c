/*
 * Players: how many Stadia controllers the dongle serves at once, and which controller is which
 * player. Player 1 is the one with the headset audio and microphone, and the first gamepad.
 *
 * NVS "players": "count" (u8, 1..STADIA_MAX_CONTROLLERS) and "id0".."id3", the device id of the
 * controller that had each player number. The Stadia core reports a new owner through
 * players_on_controller(); the next start hands the ids back so each controller gets its number
 * again (a new controller takes a free number, or the one heard from the longest ago).
 */
#include "dongle.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "players";

static char s_ids[STADIA_MAX_CONTROLLERS][STADIA_DEVICE_ID_MAX];

int players_load(const char *ids[STADIA_MAX_CONTROLLERS])
{
    int count = 1;
    nvs_handle_t h;
    if (nvs_open("players", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "count", &v) == ESP_OK && v >= 1 && v <= STADIA_MAX_CONTROLLERS) count = v;
        for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) {
            char key[4] = {'i', 'd', (char)('0' + i), 0};
            size_t len = sizeof(s_ids[i]);
            if (nvs_get_str(h, key, s_ids[i], &len) != ESP_OK) s_ids[i][0] = '\0';
        }
        nvs_close(h);
    }
    for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) ids[i] = s_ids[i];
    return count;
}

void players_save_count(int n)
{
    nvs_handle_t h;
    if (n < 1 || n > STADIA_MAX_CONTROLLERS || nvs_open("players", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "count", (uint8_t)n);
    nvs_commit(h);
    nvs_close(h);
}

void players_on_controller(int player, const char *device_id, void *arg)
{
    if (player < 0 || player >= STADIA_MAX_CONTROLLERS) return;
    nvs_handle_t h;
    if (nvs_open("players", NVS_READWRITE, &h) != ESP_OK) return;
    char key[4] = {'i', 'd', (char)('0' + player), 0};
    if (device_id && device_id[0]) {
        nvs_set_str(h, key, device_id);
        ESP_LOGI(TAG, "player %d is controller %s from now on", player + 1, device_id);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}
