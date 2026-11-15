/* The headset audio sender (audio.c), as rtc.c drives it. */
#pragma once

#include "rtc_link.h"

void audio_init(const stadia_config_t *cfg);
/* Open when SetAudioMode has been answered on a READY display link; closed when it goes away. */
void audio_set_route(bool open);
/* Send what the producer queued, on L's selected pair. Called by the rtc task. */
void audio_tick(rtc_link_t *L);
/* The rtc task's poll interval: short while frames are flowing. */
int audio_poll_ms(void);
