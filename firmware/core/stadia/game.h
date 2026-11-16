/* The game session (game.c): a second link, its input and the rumble motors. */
#pragma once

#include "rtc_link.h"
#include "stadia.h"

/* One game session per slot (player), each on its own UDP port. */
bool game_init(int slot, uint16_t port);
rtc_link_t *game_link(int slot);
/* The controller rebooted (a new display offer): forget the game session. Any task. */
void game_reset(int slot);
/* InitializationRequest (the StreamerRequest's variant 1) -> StreamerResponse into out.
 * 0 when the request is unusable. */
size_t game_initialization(int slot, const uint8_t *init, size_t n, uint8_t *out, size_t cap);
void game_tick(int slot);
/* The input series counters of stadia_stats_t. */
void game_get_stats(int slot, stadia_stats_t *st);

/* One gamepad report of a slot's controller from either link: log what changed and hand it to
 * the callback. */
void pad_report(int slot, const stadia_pad_t *pad, const char *via);
