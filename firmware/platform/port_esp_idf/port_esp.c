#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct port_event {
    SemaphoreHandle_t sem;
};

static SemaphoreHandle_t s_lock;

struct start {
    port_task_fn fn;
    void *arg;
};

static void trampoline(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    s.fn(s.arg);
    vTaskDelete(NULL);
}

static bool spawn(port_task_fn fn, void *arg, const char *name, int stack_bytes, int prio)
{
    struct start *s = malloc(sizeof(*s));
    if (!s) {
        return false;
    }
    s->fn = fn;
    s->arg = arg;
    if (xTaskCreate(trampoline, name, stack_bytes, s, prio, NULL) != pdPASS) {
        free(s);
        return false;
    }
    return true;
}

bool port_spawn(port_task_fn fn, void *arg, const char *name, int stack_bytes)
{
    return spawn(fn, arg, name, stack_bytes, 5);
}

bool port_spawn_low(port_task_fn fn, void *arg, const char *name, int stack_bytes)
{
    return spawn(fn, arg, name, stack_bytes, 4);
}

int64_t port_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int64_t port_now_us(void)
{
    return esp_timer_get_time();
}

double port_wall_time(void)
{
    return (double)STADIA_BUILD_EPOCH + esp_timer_get_time() / 1e6;
}

void *port_alloc_fast(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *port_alloc_bulk(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}

void port_sleep_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

void port_random(void *buf, size_t len)
{
    esp_fill_random(buf, len);
}

void port_mac(uint8_t mac[6])
{
    esp_read_mac(mac, ESP_MAC_BASE);
}

void port_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

port_event_t *port_event_new(void)
{
    port_event_t *ev = calloc(1, sizeof(*ev));
    ev->sem = xSemaphoreCreateBinary();
    return ev;
}

void port_event_set(port_event_t *ev)
{
    xSemaphoreGive(ev->sem);
}

bool port_event_wait(port_event_t *ev, int timeout_ms)
{
    return xSemaphoreTake(ev->sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

struct port_mutex {
    SemaphoreHandle_t sem;
};

port_mutex_t *port_mutex_new(void)
{
    port_mutex_t *m = calloc(1, sizeof(*m));
    if (m && !(m->sem = xSemaphoreCreateMutex())) {
        free(m);
        m = NULL;
    }
    return m;
}

void port_mutex_lock(port_mutex_t *m)
{
    xSemaphoreTake(m->sem, portMAX_DELAY);
}

void port_mutex_unlock(port_mutex_t *m)
{
    xSemaphoreGive(m->sem);
}

/* esp_timer: the callback runs in the esp_timer task */
struct port_timer {
    esp_timer_handle_t h;
};

port_timer_t *port_timer_new(port_task_fn fn, void *arg, const char *name)
{
    port_timer_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    const esp_timer_create_args_t args = {.callback = fn, .arg = arg, .name = name};
    if (esp_timer_create(&args, &t->h) != ESP_OK) {
        free(t);
        return NULL;
    }
    return t;
}

bool port_timer_start_once(port_timer_t *t, int64_t us)
{
    esp_timer_stop(t->h);
    return esp_timer_start_once(t->h, (uint64_t)us) == ESP_OK;
}

bool port_timer_start_periodic(port_timer_t *t, int64_t us)
{
    esp_timer_stop(t->h);
    return esp_timer_start_periodic(t->h, (uint64_t)us) == ESP_OK;
}

void port_timer_stop(port_timer_t *t)
{
    esp_timer_stop(t->h);
}

void port_log(char level, const char *tag, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    switch (level) {
    case 'E': ESP_LOGE(tag, "%s", line); break;
    case 'W': ESP_LOGW(tag, "%s", line); break;
    default: ESP_LOGI(tag, "%s", line); break;
    }
}

#include "miniz.h"

size_t port_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t cap)
{
    size_t r = tinfl_decompress_mem_to_mem(dst, cap, src, n, TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (r == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) r = tinfl_decompress_mem_to_mem(dst, cap, src, n, 0);
    return r == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED ? 0 : r;
}
