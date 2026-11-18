/* The headset audio sender (audio.c), as rtc.c drives it. */
#pragma once

#include "rtc_link.h"

void audio_init(const stadia_config_t *cfg);
/* A player's route: open when SetAudioMode has been answered on its READY display link; closed
 * when the link goes away. Frames are encoded while any player's is open. */
void audio_set_route(int slot, bool open);
/* Send what the producer queued to every player whose route is open, each on its display link
 * (links[slot], n of them). Called by the rtc task. */
void audio_tick(rtc_link_t *const *links, int n);
/* The rtc task's poll interval: short while frames are flowing. */
int audio_poll_ms(void);
/* The headset audio's part of stadia_get_stats(). */
void audio_get_stats(stadia_stats_t *st);
