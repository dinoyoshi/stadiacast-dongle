/* The game session (game.c): a second link, its input and the rumble motors. */
#pragma once

#include "rtc_link.h"
#include "stadia.h"

bool game_init(uint16_t port);
rtc_link_t *game_link(void);
/* The controller rebooted (a new display offer): forget the game session. */
void game_reset(void);
/* InitializationRequest (the StreamerRequest's variant 1) -> StreamerResponse into out.
 * 0 when the request is unusable. */
size_t game_initialization(const uint8_t *init, size_t n, uint8_t *out, size_t cap);
void game_tick(void);
/* The input series counters of stadia_stats_t. */
void game_get_stats(stadia_stats_t *st);

/* One gamepad report from either link: log what changed and hand it to the callback. */
void pad_report(const stadia_pad_t *pad, const char *via);
