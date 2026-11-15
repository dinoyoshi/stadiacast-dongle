#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
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

bool port_spawn(port_task_fn fn, void *arg, const char *name, int stack_bytes)
{
    struct start *s = malloc(sizeof(*s));
    if (!s) {
        return false;
    }
    s->fn = fn;
    s->arg = arg;
    if (xTaskCreate(trampoline, name, stack_bytes, s, 5, NULL) != pdPASS) {
        free(s);
        return false;
    }
    return true;
}

int64_t port_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

double port_wall_time(void)
{
    return (double)STADIA_BUILD_EPOCH + esp_timer_get_time() / 1e6;
}

void port_sleep_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

void port_random(void *buf, size_t len)
{
    esp_fill_random(buf, len);
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
