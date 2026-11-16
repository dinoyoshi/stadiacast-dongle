/*
 * The FireBeetle 2 ESP32-P4's side of the console (dongle_board.h): the USB serial port
 * (usb_pad's usb_console.c), the chip and firmware for "info", CPU per core, task list, memory
 * and the hotspot's signal for "stats", and the log tee (ESP-IDF's log, copied to the console
 * while the host asked for it with "logs on").
 */
#include "dongle_board.h"
#include "dongle_core.h"
#include "usb_pad.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define LOG_LINE 240
#define MAX_TASKS 40

/* CPU figures need FreeRTOS's run time stats (CONFIG_DONGLE_CONSOLE selects them) */
#define CONSOLE_CPU (configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS)

static struct {
    /* the log tee */
    vprintf_like_t prev_vprintf;
    SemaphoreHandle_t log_lock;
    char *log_buf;
    /* CPU: the run time counters at the last snapshot */
    TaskStatus_t *tasks;
    struct prev_run {
        UBaseType_t num;
        configRUN_TIME_COUNTER_TYPE run;
    } *prev; /* MAX_TASKS */
    int nprev;
    int64_t prev_us;       /* the last per-core figures */
    int64_t tasks_prev_us; /* the last task list */
    configRUN_TIME_COUNTER_TYPE idle_prev[2];
    int cpu[2]; /* % busy, -1 unknown */
    int64_t wifi_at;
    int rssi, stations;
    bool rssi_ok;
} B = {.cpu = {-1, -1}};

/* ---- the link ------------------------------------------------------------------------- */

bool board_console_active(void)
{
    return usb_console_active();
}

bool board_console_write(const char *s, size_t len)
{
    return usb_console_write(s, len);
}

uint32_t board_console_dropped(void)
{
    return usb_console_dropped();
}

bool board_console_read_line(char *out, uint32_t timeout_ms)
{
    return usb_console_read_line(out, timeout_ms);
}

void board_console_set_raw(board_console_raw_cb_t cb)
{
    usb_console_set_raw(cb);
}

void board_restart(void)
{
    esp_restart();
}

/* ---- info ----------------------------------------------------------------------------- */

void board_info(board_info_t *out)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    *out = (board_info_t){
        .fw = app->version,
        .project = app->project_name,
        .date = app->date,
        .time = app->time,
        .sdk = app->idf_ver,
        .cores = chip.cores,
        .ssid = CONFIG_DONGLE_SSID,
        .channel = CONFIG_DONGLE_CHANNEL,
    };
    snprintf(out->chip, sizeof(out->chip), "ESP32-P4 rev v%d.%d", chip.revision / 100, chip.revision % 100);
    esp_read_mac(out->mac, ESP_MAC_BASE);
}

/* ---- stats ---------------------------------------------------------------------------- */

/* CPU per core from the idle tasks' run time since the last call. Cheap: a few reads under the
 * kernel lock, once a second. */
static void cpu_cores(void)
{
#if CONSOLE_CPU
    int64_t now = esp_timer_get_time();
    int64_t wall = now - B.prev_us;
    bool have_prev = B.prev_us != 0 && wall > 0;
    for (int c = 0; c < portNUM_PROCESSORS && c < 2; c++) {
        configRUN_TIME_COUNTER_TYPE run = ulTaskGetIdleRunTimeCounterForCore(c);
        if (have_prev) {
            int64_t idle_us = (int64_t)(configRUN_TIME_COUNTER_TYPE)(run - B.idle_prev[c]);
            int busy = 100 - (int)(idle_us * 100 / wall);
            B.cpu[c] = busy < 0 ? 0 : busy > 100 ? 100 : busy;
        }
        B.idle_prev[c] = run;
    }
    B.prev_us = now;
#endif
}

/* The controller's signal, from the C6 (an RPC over SDIO): every 2 s at most. */
static void wifi_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (now - B.wifi_at < 2000000) return;
    B.wifi_at = now;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        B.rssi_ok = false;
        return;
    }
    B.stations = list.num;
    B.rssi_ok = list.num > 0;
    int best = -127;
    for (int i = 0; i < list.num; i++) {
        if (list.sta[i].rssi > best) best = list.sta[i].rssi;
    }
    B.rssi = best;
}

void board_stats(board_stats_t *out)
{
    wifi_poll();
    cpu_cores();
    *out = (board_stats_t){
        .cpu = {B.cpu[0], B.cpu[1]},
        .heap = {(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)},
        .rssi_ok = B.rssi_ok,
        .rssi = B.rssi,
        .stations = B.stations,
    };
}

void board_stats_reset(void)
{
    B.prev_us = B.tasks_prev_us = 0;
}

/* Each task's share of a core since the last list. uxTaskGetSystemState() holds the kernel's
 * critical section (interrupts off) while it measures every task's free stack: about 6 ms on
 * the board, long enough for the USB interrupt to miss ~50 headphone packets (usb_audio.c). So
 * the console calls this every 2 s only while the headphones are idle, and once on "tasks". */
int board_tasks_json(char *out, int cap)
{
    int len = 0;
#if CONSOLE_CPU
    if (!B.tasks) return 0;
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t n = uxTaskGetSystemState(B.tasks, MAX_TASKS, &total);
    int64_t now = esp_timer_get_time();
    int64_t wall = now - B.tasks_prev_us;
    if (B.tasks_prev_us != 0 && wall > 0) {
        len = snprintf(out, cap, "{\"t\":\"tasks\",\"ms\":%d,\"list\":[", (int)(wall / 1000));
        bool first = true;
        for (UBaseType_t i = 0; i < n && len < cap - 80; i++) {
            configRUN_TIME_COUNTER_TYPE before = 0;
            bool known = false;
            for (int j = 0; j < B.nprev; j++) {
                if (B.prev[j].num == B.tasks[i].xTaskNumber) {
                    before = B.prev[j].run;
                    known = true;
                    break;
                }
            }
            if (!known) continue;
            uint32_t pm = (uint32_t)((int64_t)(configRUN_TIME_COUNTER_TYPE)(B.tasks[i].ulRunTimeCounter - before) * 1000 / wall);
            len += snprintf(out + len, cap - len, "%s[\"%s\",%u,%u,%u]", first ? "" : ",", B.tasks[i].pcTaskName,
                            (unsigned)pm, (unsigned)B.tasks[i].uxCurrentPriority,
                            (unsigned)B.tasks[i].usStackHighWaterMark);
            first = false;
        }
        if (len < cap - 3) {
            len += snprintf(out + len, cap - len, "]}\n");
        } else {
            len = 0;
        }
    }
    B.nprev = 0;
    for (UBaseType_t i = 0; i < n && B.nprev < MAX_TASKS; i++) {
        B.prev[B.nprev].num = B.tasks[i].xTaskNumber;
        B.prev[B.nprev].run = B.tasks[i].ulRunTimeCounter;
        B.nprev++;
    }
    B.tasks_prev_us = now;
#endif
    return len;
}

/* ---- the log tee ---------------------------------------------------------------------- */

static int log_vprintf(const char *fmt, va_list ap)
{
    if (console_logs_on() && B.log_buf && usb_console_active() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(B.log_lock, 0) == pdTRUE) {
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(B.log_buf, LOG_LINE, fmt, copy);
        va_end(copy);
        if (n > 0) {
            if (n >= LOG_LINE) { /* cut: still one line */
                n = LOG_LINE - 1;
                B.log_buf[n - 1] = '\n';
            }
            if (B.log_buf[0] == '{') B.log_buf[0] = '('; /* never mistaken for a message */
            usb_console_write(B.log_buf, (size_t)n);
        }
        xSemaphoreGive(B.log_lock);
    }
    return B.prev_vprintf(fmt, ap);
}

/* With console_start: the task list's buffers, and the log tee. */
void board_console_start(void)
{
    B.log_buf = malloc(LOG_LINE);
    B.log_lock = xSemaphoreCreateMutex();
#if CONSOLE_CPU
    B.tasks = malloc(sizeof(TaskStatus_t) * MAX_TASKS);
    B.prev = malloc(sizeof(*B.prev) * MAX_TASKS);
    if (!B.prev) B.tasks = NULL;
#endif
    if (!B.log_lock) return;
    B.prev_vprintf = esp_log_set_vprintf(log_vprintf);
}
