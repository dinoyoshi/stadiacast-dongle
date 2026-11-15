/*
 * The few OS services the Stadiacast core needs, so the same sources build for the ESP32-P4
 * (FreeRTOS + lwIP, port_esp.c) and for a Linux host test against the Python fake controller
 * (pthreads + BSD sockets, port_posix.c). Sockets are plain BSD calls on both.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*port_task_fn)(void *arg);

/* Start fn(arg) in a task (FreeRTOS) or detached thread (host). */
bool port_spawn(port_task_fn fn, void *arg, const char *name, int stack_bytes);
/* The same at a lower priority than the network task (FreeRTOS 4 instead of 5; the host ignores it):
 * work that can wait behind the links and the headphones' encoder. */
bool port_spawn_low(port_task_fn fn, void *arg, const char *name, int stack_bytes);

/* Milliseconds since boot, monotonic. */
int64_t port_now_ms(void);
int64_t port_now_us(void);

/* Unix time in seconds. The dongle has no clock source, so it counts from the build time;
 * the controller checks neither certificate dates nor anything else against it. */
double port_wall_time(void);

void port_sleep_ms(int ms);
/* Memory the CPU reads fastest (internal RAM on the board, not PSRAM); NULL if there is none left. */
void *port_alloc_fast(size_t n);
void port_random(void *buf, size_t len);
/* Inflate a zlib (or else raw deflate) stream into dst. Returns the size, 0 on failure. */
size_t port_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

/* One global lock for the controller session. */
void port_lock(void);
void port_unlock(void);

/* An auto-reset event: set() wakes one wait(). wait() returns false on timeout. */
typedef struct port_event port_event_t;
port_event_t *port_event_new(void);
void port_event_set(port_event_t *ev);
bool port_event_wait(port_event_t *ev, int timeout_ms);

/* Logging, one line per call. */
void port_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define SLOGI(tag, ...) port_log('I', tag, __VA_ARGS__)
#define SLOGW(tag, ...) port_log('W', tag, __VA_ARGS__)
#define SLOGE(tag, ...) port_log('E', tag, __VA_ARGS__)
