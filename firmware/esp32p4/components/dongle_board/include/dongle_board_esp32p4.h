/* What this board's start-up (main.c) calls in the dongle_board component beyond dongle_board.h. */
#pragma once

/* With console_start: the task list's buffers, and the log tee (ESP-IDF's log to the console
 * while the host asked for it). */
void board_console_start(void);
