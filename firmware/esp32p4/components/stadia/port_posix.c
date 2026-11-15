/* Host build of the port layer, for the loopback test against tests/fake_controller.py. */
#include "stadia_port.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>

struct port_event {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool set;
};

static pthread_mutex_t s_lock;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void init_lock(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_lock, &a);
}

struct start {
    port_task_fn fn;
    void *arg;
};

static void *trampoline(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

bool port_spawn(port_task_fn fn, void *arg, const char *name, int stack_bytes)
{
    (void)name;
    (void)stack_bytes;
    struct start *s = malloc(sizeof(*s));
    s->fn = fn;
    s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, trampoline, s) != 0) {
        free(s);
        return false;
    }
    pthread_detach(t);
    return true;
}

int64_t port_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t port_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

double port_wall_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void port_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

void port_random(void *buf, size_t len)
{
    if (getrandom(buf, len, 0) != (ssize_t)len) {
        abort();
    }
}

void port_lock(void)
{
    pthread_once(&s_once, init_lock);
    pthread_mutex_lock(&s_lock);
}

void port_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

port_event_t *port_event_new(void)
{
    port_event_t *ev = calloc(1, sizeof(*ev));
    pthread_mutex_init(&ev->mu, NULL);
    pthread_cond_init(&ev->cv, NULL);
    return ev;
}

void port_event_set(port_event_t *ev)
{
    pthread_mutex_lock(&ev->mu);
    ev->set = true;
    pthread_cond_signal(&ev->cv);
    pthread_mutex_unlock(&ev->mu);
}

bool port_event_wait(port_event_t *ev, int timeout_ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&ev->mu);
    int rc = 0;
    while (!ev->set && rc == 0) {
        rc = pthread_cond_timedwait(&ev->cv, &ev->mu, &until);
    }
    bool got = ev->set;
    ev->set = false;
    pthread_mutex_unlock(&ev->mu);
    return got;
}

void port_log(char level, const char *tag, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "%c (%lld.%03ld) %s: %s\n", level, (long long)ts.tv_sec, ts.tv_nsec / 1000000, tag, line);
}

#include <zlib.h>

size_t port_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    for (int wbits = MAX_WBITS; wbits >= -MAX_WBITS; wbits -= 2 * MAX_WBITS) {
        z_stream z = {0};
        if (inflateInit2(&z, wbits) != Z_OK) return 0;
        z.next_in = (Bytef *)src, z.avail_in = (uInt)n;
        z.next_out = dst, z.avail_out = (uInt)cap;
        int rc = inflate(&z, Z_FINISH);
        size_t out = z.total_out;
        inflateEnd(&z);
        if (rc == Z_STREAM_END) return out;
    }
    return 0;
}
