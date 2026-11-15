/* The controller's microphone (mic.c), as rtc.c and game.c feed it. */
#pragma once

#include "rtc_link.h"

/* The decoder's queue and task. False if there is no memory for them (no microphone then). */
bool mic_init(void);
/* One decrypted RTP packet the controller sent as media on a link (0 display, 1 game).
 * Called by the rtc task; only copies. */
void mic_on_rtp(rtc_link_t *L, int link, const uint8_t *rtp, size_t n);
