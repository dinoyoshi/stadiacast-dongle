/* The controller's microphone (mic.c), as rtc.c and game.c feed it. */
#pragma once

#include "rtc_link.h"

/* The decoder's queue and task. False if there is no memory for them (no microphone then). */
bool mic_init(void);
/* One decrypted RTP packet the controller sent as media on a link (0 display, 1 game).
 * Called by the rtc task; only copies. */
void mic_on_rtp(rtc_link_t *L, int link, const uint8_t *rtp, size_t n);
/* When the last packet was received (port_now_ms), 0 before any. Push to talk drops packets
 * while nobody talks, but they still count here. */
int64_t mic_last_packet_ms(int slot);
/* A player's Assistant button, from game.c's input path: held or not (false when the pad goes away). */
void mic_set_talk(int slot, bool held);
/* Held, or let go less than the tail ago: the microphone is wanted (push to talk). */
bool mic_talking(int slot);
/* The decoder has work: push to talk held, or speech (not just DTX) in the last 300 ms. The
 * headphones' encoder lowers its complexity meanwhile (audio.c). */
bool mic_busy(void);
/* A player's headset jack: that player's mic gain follows it. From rtc.c. */
void mic_set_headset(int slot, bool in);
/* The microphone's part of stadia_get_stats(). */
void mic_get_stats(stadia_stats_t *st);
