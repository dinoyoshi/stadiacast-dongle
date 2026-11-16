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

void *port_alloc_fast(size_t n)
{
    return malloc(n);
}

void *port_alloc_bulk(size_t n)
{
    return calloc(1, n);
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

/* A fixed, locally administered address: the host has no board MAC of its own to give. */
void port_mac(uint8_t mac[6])
{
    static const uint8_t m[6] = {0x02, 0x53, 0x54, 0x44, 0x41, 0x01};
    for (int i = 0; i < 6; i++) mac[i] = m[i];
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

struct port_mutex {
    pthread_mutex_t mu;
};

port_mutex_t *port_mutex_new(void)
{
    port_mutex_t *m = calloc(1, sizeof(*m));
    if (m) pthread_mutex_init(&m->mu, NULL);
    return m;
}

void port_mutex_lock(port_mutex_t *m)
{
    pthread_mutex_lock(&m->mu);
}

void port_mutex_unlock(port_mutex_t *m)
{
    pthread_mutex_unlock(&m->mu);
}

/* One thread per timer, waiting on a condition variable for the next expiry or a change. */
struct port_timer {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    port_task_fn fn;
    void *arg;
    int64_t due_us, period_us; /* due 0: stopped */
};

static void *timer_thread(void *p)
{
    port_timer_t *t = p;
    pthread_mutex_lock(&t->mu);
    for (;;) {
        if (!t->due_us) {
            pthread_cond_wait(&t->cv, &t->mu);
            continue;
        }
        int64_t now = port_now_us();
        if (now < t->due_us) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            int64_t ns = until.tv_nsec + (t->due_us - now) * 1000;
            until.tv_sec += ns / 1000000000L;
            until.tv_nsec = ns % 1000000000L;
            pthread_cond_timedwait(&t->cv, &t->mu, &until);
            continue;
        }
        t->due_us = t->period_us ? t->due_us + t->period_us : 0;
        pthread_mutex_unlock(&t->mu);
        t->fn(t->arg);
        pthread_mutex_lock(&t->mu);
    }
    return NULL;
}

port_timer_t *port_timer_new(port_task_fn fn, void *arg, const char *name)
{
    port_timer_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->fn = fn, t->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, timer_thread, t) != 0) {
        free(t);
        return NULL;
    }
    pthread_detach(th);
    return t;
}

static bool timer_set(port_timer_t *t, int64_t us, int64_t period)
{
    pthread_mutex_lock(&t->mu);
    t->due_us = port_now_us() + (us > 0 ? us : 1);
    t->period_us = period;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
    return true;
}

bool port_timer_start_once(port_timer_t *t, int64_t us)
{
    return timer_set(t, us, 0);
}

bool port_timer_start_periodic(port_timer_t *t, int64_t us)
{
    return timer_set(t, us, us);
}

void port_timer_stop(port_timer_t *t)
{
    pthread_mutex_lock(&t->mu);
    t->due_us = 0;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mu);
}

struct port_stream {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint8_t *buf;
    size_t size, head, len;
};

port_stream_t *port_stream_new(size_t size, bool bulk)
{
    port_stream_t *s = calloc(1, sizeof(*s));
    if (!s || !(s->buf = malloc(size))) {
        free(s);
        return NULL;
    }
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->size = size;
    return s;
}

bool port_stream_send(port_stream_t *s, const void *data, size_t n)
{
    pthread_mutex_lock(&s->mu);
    bool fits = s->size - s->len >= n;
    for (size_t i = 0; fits && i < n; i++) s->buf[(s->head + s->len + i) % s->size] = ((const uint8_t *)data)[i];
    if (fits) {
        s->len += n;
        pthread_cond_signal(&s->cv);
    }
    pthread_mutex_unlock(&s->mu);
    return fits;
}

size_t port_stream_receive(port_stream_t *s, void *buf, size_t max, int timeout_ms)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    int64_t ns = until.tv_nsec + (int64_t)timeout_ms * 1000000;
    until.tv_sec += ns / 1000000000L;
    until.tv_nsec = ns % 1000000000L;
    pthread_mutex_lock(&s->mu);
    while (!s->len && pthread_cond_timedwait(&s->cv, &s->mu, &until) == 0) {
    }
    size_t n = s->len < max ? s->len : max;
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = s->buf[(s->head + i) % s->size];
    s->head = (s->head + n) % s->size;
    s->len -= n;
    pthread_mutex_unlock(&s->mu);
    return n;
}

void port_stream_reset(port_stream_t *s)
{
    pthread_mutex_lock(&s->mu);
    s->head = s->len = 0;
    pthread_mutex_unlock(&s->mu);
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

size_t port_gunzip(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    z_stream z = {0};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return 0; /* 16: gzip only */
    z.next_in = (Bytef *)src, z.avail_in = (uInt)n;
    z.next_out = dst, z.avail_out = (uInt)cap;
    int rc = inflate(&z, Z_FINISH);
    size_t out = z.total_out;
    inflateEnd(&z);
    return rc == Z_STREAM_END ? out : 0;
}

bool port_spawn_low(port_task_fn fn, void *arg, const char *name, int stack_bytes)
{
    return port_spawn(fn, arg, name, stack_bytes);
}

bool port_spawn_prio(port_task_fn fn, void *arg, const char *name, int stack_bytes, int prio)
{
    return port_spawn(fn, arg, name, stack_bytes);
}
