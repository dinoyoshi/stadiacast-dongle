/*
 * The console: live stats and settings over the USB serial port that sits next to the gamepad
 * (usb_console.c), for the web console in firmware/esp32p4/console/ (Chrome or Edge, Web Serial)
 * or a terminal.
 *
 * To the host, one line each, only while the host has the port open:
 *   {"t":"info",...}      firmware, chip, hotspot; after "hello"
 *   {"t":"settings",...}  the settings; after "hello", "get" and every change
 *   {"t":"stats",...}     once a second: the controller session (with its battery and the link
 *                         quality: keepalive round trip, input series lost), headset audio,
 *                         microphone, USB, CPU per core and memory (counters from power-up: the
 *                         page takes the differences)
 *   {"t":"tasks",...}     each task's share of a core since the last one: every 2 s while the
 *                         headphones are idle, and on "tasks" (see tasks_snapshot)
 *   {"t":"pad",...}       a controller's input as the host gets it ("p": its player, 0 = player 1),
 *                         at most 20 times a second per player
 *   {"t":"ok"|"err",...}  the answer to a command
 *   anything else         the firmware's log, while "logs on"
 *   {"t":"rumble",...}    the motors' levels (0..65535) and the host's (0..255), when they change
 * From the host, a line each: hello | get | tasks | set <key> <value> | defaults | logs on|off | reboot
 *   | rumble <left 0-255> <right 0-255> [ms]  (a test: through the rumble settings, 1 s by
 *   default, 5 s at most; "rumble 0 0" stops it)
 *   | ota [begin <size> <sha256>]  (a firmware update: ota.c, which also sends {"t":"ota",...})
 *   | forget <player 1-4>  (that player number goes to the next new controller)
 *   | pair [stop]  (give a controller in Bluetooth setup mode the hotspot: pair.c; progress comes
 *     as {"t":"pair","state":...,"msg":...} whenever it changes)
 *   keys: mode 0-3, gain -12..0 (dB before the Opus encoder), complexity 1-10,
 *   mic_complexity 1-10 (the complexity while the mic runs, when lower), bitrate 32-160 (Opus kbit/s), mic 0/1 (after a
 *   restart), ptt 0/1 (push to talk), rumble 0/1, rumble_left and rumble_right 0-200 (% strength),
 *   players 1-4 (controllers at once, after a restart)
 * The stats carry "players": each player's controller (its id, address, link state, battery).
 *
 * The settings live in NVS ("console"), the mode where pad_mode.c keeps it. All of this costs the
 * board nothing while no one has the port open but a 100 ms wake-up of a low-priority task.
 */
#include "dongle.h"
#include "stadia.h"
#include "usb_pad.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "console";

#define OUT_MAX 3072
#define LOG_LINE 240
#define MAX_TASKS 40
#define PAD_MIN_US (50 * 1000)

/* CPU figures need FreeRTOS's run time stats (CONFIG_DONGLE_CONSOLE selects them) */
#define CONSOLE_CPU (configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS)

#ifndef CONFIG_DONGLE_OPUS_COMPLEXITY
#define CONFIG_DONGLE_OPUS_COMPLEXITY 8
#endif
#ifndef CONFIG_DONGLE_OPUS_MIC_COMPLEXITY
#define CONFIG_DONGLE_OPUS_MIC_COMPLEXITY 4
#endif
#ifndef CONFIG_DONGLE_OPUS_BITRATE
#define CONFIG_DONGLE_OPUS_BITRATE 96
#endif

static struct {
    dongle_settings_t set;
    volatile int32_t gain_q15;
    bool started;
    char *out; /* OUT_MAX: the task's line buffer */
    /* the log tee */
    vprintf_like_t prev_vprintf;
    volatile bool logs;
    SemaphoreHandle_t log_lock;
    char *log_buf;
    /* the pads, from the link task */
    portMUX_TYPE pad_mux;
    usb_pad_state_t pad[STADIA_MAX_CONTROLLERS];
    volatile uint32_t pad_new; /* one bit per player */
    int64_t pad_sent_us[STADIA_MAX_CONTROLLERS];
    int players, players_running; /* saved (next start), this start */
    /* CPU: the run time counters at the last snapshot */
    TaskStatus_t *tasks;
    struct prev_run {
        UBaseType_t num;
        configRUN_TIME_COUNTER_TYPE run;
    } *prev; /* MAX_TASKS */
    int nprev;
    int64_t prev_us;       /* the last per-core figures */
    int64_t tasks_prev_us; /* the last task list */
    volatile bool tasks_req;
    int tasks_again; /* stats ticks until a list asked for with "tasks" */
    configRUN_TIME_COUNTER_TYPE idle_prev[2];
    int cpu[2]; /* % busy, -1 unknown */
    int64_t wifi_at;
    int rssi, stations;
    bool rssi_ok;
} K = {.pad_mux = portMUX_INITIALIZER_UNLOCKED, .cpu = {-1, -1}};

/* ---- settings ---------------------------------------------------------------------- */

static void defaults(dongle_settings_t *s)
{
    *s = (dongle_settings_t){
        .gain_db = 0,
        .complexity = CONFIG_DONGLE_OPUS_COMPLEXITY,
        .mic_complexity = CONFIG_DONGLE_OPUS_MIC_COMPLEXITY,
        .kbps = CONFIG_DONGLE_OPUS_BITRATE,
#if CONFIG_DONGLE_USB_MIC
        .mic = true,
#endif
#if CONFIG_DONGLE_MIC_PUSH_TO_TALK
        .ptt = true,
#endif
        .rumble = true,
        .rumble_pct = {100, 100},
    };
}

static void apply_rumble(void)
{
    rumble_config(K.set.rumble, K.set.rumble_pct[0], K.set.rumble_pct[1]);
}

static void apply_gain(int gain_db)
{
    K.gain_q15 = (int32_t)lroundf(32768.0f * powf(10.0f, gain_db / 20.0f));
}

void settings_load(dongle_settings_t *out)
{
    dongle_settings_t s;
    defaults(&s);
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        int8_t i8;
        uint8_t u8;
        if (nvs_get_i8(h, "gain", &i8) == ESP_OK && i8 >= -12 && i8 <= 0) s.gain_db = i8;
        if (nvs_get_u8(h, "cplx", &u8) == ESP_OK && u8 >= 1 && u8 <= 10) s.complexity = u8;
        if (nvs_get_u8(h, "mcplx", &u8) == ESP_OK && u8 >= 1 && u8 <= 10) s.mic_complexity = u8;
        if (nvs_get_u8(h, "kbps", &u8) == ESP_OK && u8 >= STADIA_AUDIO_KBPS_MIN && u8 <= STADIA_AUDIO_KBPS_MAX)
            s.kbps = u8;
        if (nvs_get_u8(h, "mic", &u8) == ESP_OK) s.mic = u8 != 0;
        if (nvs_get_u8(h, "ptt", &u8) == ESP_OK) s.ptt = u8 != 0;
        if (nvs_get_u8(h, "rmb", &u8) == ESP_OK) s.rumble = u8 != 0;
        if (nvs_get_u8(h, "rmb_l", &u8) == ESP_OK && u8 <= RUMBLE_PCT_MAX) s.rumble_pct[0] = u8;
        if (nvs_get_u8(h, "rmb_r", &u8) == ESP_OK && u8 <= RUMBLE_PCT_MAX) s.rumble_pct[1] = u8;
        nvs_close(h);
    }
#if !CONFIG_DONGLE_USB_MIC
    s.mic = false;
#endif
    K.set = s;
    apply_gain(s.gain_db);
    apply_rumble();
    *out = s;
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "gain", K.set.gain_db);
    nvs_set_u8(h, "cplx", K.set.complexity);
    nvs_set_u8(h, "mcplx", K.set.mic_complexity);
    nvs_set_u8(h, "kbps", K.set.kbps);
    nvs_set_u8(h, "mic", K.set.mic);
    nvs_set_u8(h, "ptt", K.set.ptt);
    nvs_set_u8(h, "rmb", K.set.rumble);
    nvs_set_u8(h, "rmb_l", K.set.rumble_pct[0]);
    nvs_set_u8(h, "rmb_r", K.set.rumble_pct[1]);
    nvs_commit(h);
    nvs_close(h);
}

int32_t console_gain_q15(void)
{
    return K.gain_q15 ? K.gain_q15 : 32768;
}

/* ---- out ----------------------------------------------------------------------------- */

static void send_line(const char *s, int n)
{
    if (n <= 0) return;
    if (n >= OUT_MAX - 1) n = OUT_MAX - 2; /* cut: the page drops what it cannot parse */
    usb_console_write(s, (size_t)n);
}

/* printf into K.out and send, with the newline */
static void sendf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(K.out, OUT_MAX - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > OUT_MAX - 2) n = OUT_MAX - 2;
    K.out[n++] = '\n';
    send_line(K.out, n);
}

static void send_settings(void)
{
    sendf("{\"t\":\"settings\",\"mode\":%d,\"gain\":%d,\"complexity\":%d,\"mic_complexity\":%d,\"bitrate\":%d,\"mic\":%s,\"mic_running\":%s,\"ptt\":%s,"
          "\"rumble\":%s,\"rumble_left\":%d,\"rumble_right\":%d,\"logs\":%s,\"players\":%d,\"players_running\":%d,"
          "\"usb_players\":%d}",
          (int)usb_pad_mode(), K.set.gain_db, K.set.complexity, K.set.mic_complexity, K.set.kbps, K.set.mic ? "true" : "false",
          K.set.mic_running ? "true" : "false", stadia_mic_push_to_talk() ? "true" : "false",
          K.set.rumble ? "true" : "false", K.set.rumble_pct[0], K.set.rumble_pct[1], K.logs ? "true" : "false",
          K.players, K.players_running, usb_pad_players());
}

static void send_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
    sendf("{\"t\":\"info\",\"fw\":\"%s\",\"project\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\",\"chip\":\"ESP32-P4 "
          "rev v%d.%d\",\"cores\":%d,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ssid\":\"%s\",\"channel\":%d,"
          "\"audio\":%s,\"modes\":[\"%s\",\"%s\",\"%s\",\"%s\"]}",
          app->version, app->project_name, app->date, app->time, app->idf_ver, chip.revision / 100, chip.revision % 100,
          chip.cores, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], CONFIG_DONGLE_SSID, CONFIG_DONGLE_CHANNEL,
#if CONFIG_DONGLE_USB_AUDIO
          "true",
#else
          "false",
#endif
          usb_pad_mode_name(USB_PAD_XINPUT), usb_pad_mode_name(USB_PAD_DS4), usb_pad_mode_name(USB_PAD_SWITCH),
          usb_pad_mode_name(USB_PAD_HID));
}

/* ---- the log tee ----------------------------------------------------------------------- */

static int log_vprintf(const char *fmt, va_list ap)
{
    if (K.logs && K.log_buf && usb_console_active() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(K.log_lock, 0) == pdTRUE) {
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(K.log_buf, LOG_LINE, fmt, copy);
        va_end(copy);
        if (n > 0) {
            if (n >= LOG_LINE) { /* cut: still one line */
                n = LOG_LINE - 1;
                K.log_buf[n - 1] = '\n';
            }
            if (K.log_buf[0] == '{') K.log_buf[0] = '('; /* never mistaken for a message */
            usb_console_write(K.log_buf, (size_t)n);
        }
        xSemaphoreGive(K.log_lock);
    }
    return K.prev_vprintf(fmt, ap);
}

/* ---- stats -------------------------------------------------------------------------- */

void console_pad(int player, const usb_pad_state_t *st)
{
    if (!K.started || player < 0 || player >= STADIA_MAX_CONTROLLERS) return;
    portENTER_CRITICAL(&K.pad_mux);
    K.pad[player] = *st;
    K.pad_new |= 1u << player;
    portEXIT_CRITICAL(&K.pad_mux);
}

static void send_pad(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < STADIA_MAX_CONTROLLERS; i++) {
        if (!(K.pad_new & (1u << i)) || now - K.pad_sent_us[i] < PAD_MIN_US) continue;
        usb_pad_state_t p;
        portENTER_CRITICAL(&K.pad_mux);
        p = K.pad[i];
        K.pad_new &= ~(1u << i);
        portEXIT_CRITICAL(&K.pad_mux);
        K.pad_sent_us[i] = now;
        sendf("{\"t\":\"pad\",\"p\":%d,\"b\":%u,\"x\":%u,\"lt\":%u,\"rt\":%u,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d}", i,
              p.buttons, p.extra, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry);
    }
}

/* Each player's controller, as one JSON array (into out, cap bytes); its length. */
static int players_json(char *out, int cap)
{
    stadia_player_t pl[STADIA_MAX_CONTROLLERS];
    int n = stadia_get_players(pl, STADIA_MAX_CONTROLLERS);
    int len = snprintf(out, cap, "[");
    for (int i = 0; i < n && len < cap - 200; i++) {
        const stadia_player_t *p = &pl[i];
        char bat[12] = "null", ping[12] = "null";
        if (p->battery_pct >= 0) snprintf(bat, sizeof(bat), "%d", p->battery_pct);
        if (p->ping_us >= 0) snprintf(ping, sizeof(ping), "%d", (int)p->ping_us);
        len += snprintf(out + len, cap - len,
                        "%s{\"known\":%s,\"id\":\"%s\",\"ip\":\"%s\",\"seen\":%d,\"state\":\"%s\",\"disp\":%s,\"game\":%s,"
                        "\"bat\":%s,\"ping\":%s,\"in\":[%u,%u],\"usb\":%s}",
                        i ? "," : "", p->known ? "true" : "false", p->device_id, p->ip, (int)p->seen_s,
                        p->display_state ? p->display_state : "none", p->display_up ? "true" : "false",
                        p->game_up ? "true" : "false", bat, ping, (unsigned)p->input_series, (unsigned)p->input_gaps,
                        i < usb_pad_players() ? "true" : "false");
    }
    if (len < cap - 1) len += snprintf(out + len, cap - len, "]");
    return len;
}

/* CPU per core from the idle tasks' run time since the last call. Cheap: a few reads under the
 * kernel lock, once a second. */
static void cpu_cores(void)
{
#if CONSOLE_CPU
    int64_t now = esp_timer_get_time();
    int64_t wall = now - K.prev_us;
    bool have_prev = K.prev_us != 0 && wall > 0;
    for (int c = 0; c < portNUM_PROCESSORS && c < 2; c++) {
        configRUN_TIME_COUNTER_TYPE run = ulTaskGetIdleRunTimeCounterForCore(c);
        if (have_prev) {
            int64_t idle_us = (int64_t)(configRUN_TIME_COUNTER_TYPE)(run - K.idle_prev[c]);
            int busy = 100 - (int)(idle_us * 100 / wall);
            K.cpu[c] = busy < 0 ? 0 : busy > 100 ? 100 : busy;
        }
        K.idle_prev[c] = run;
    }
    K.prev_us = now;
#endif
}

/* Each task's share of a core since the last list. uxTaskGetSystemState() holds the kernel's
 * critical section (interrupts off) while it measures every task's free stack: about 6 ms on
 * the board, long enough for the USB interrupt to miss ~50 headphone packets (usb_audio.c). So
 * it runs every 2 s only while the headphones are idle, and once on "tasks" from the host. */
static void tasks_snapshot(void)
{
#if CONSOLE_CPU
    if (!K.tasks) return;
    configRUN_TIME_COUNTER_TYPE total;
    UBaseType_t n = uxTaskGetSystemState(K.tasks, MAX_TASKS, &total);
    int64_t now = esp_timer_get_time();
    int64_t wall = now - K.tasks_prev_us;
    if (K.tasks_prev_us != 0 && wall > 0) {
        int len = snprintf(K.out, OUT_MAX, "{\"t\":\"tasks\",\"ms\":%d,\"list\":[", (int)(wall / 1000));
        bool first = true;
        for (UBaseType_t i = 0; i < n && len < OUT_MAX - 80; i++) {
            configRUN_TIME_COUNTER_TYPE before = 0;
            bool known = false;
            for (int j = 0; j < K.nprev; j++) {
                if (K.prev[j].num == K.tasks[i].xTaskNumber) {
                    before = K.prev[j].run;
                    known = true;
                    break;
                }
            }
            if (!known) continue;
            uint32_t pm = (uint32_t)((int64_t)(configRUN_TIME_COUNTER_TYPE)(K.tasks[i].ulRunTimeCounter - before) * 1000 / wall);
            len += snprintf(K.out + len, OUT_MAX - len, "%s[\"%s\",%u,%u,%u]", first ? "" : ",", K.tasks[i].pcTaskName,
                            (unsigned)pm, (unsigned)K.tasks[i].uxCurrentPriority,
                            (unsigned)K.tasks[i].usStackHighWaterMark);
            first = false;
        }
        if (len < OUT_MAX - 3) {
            len += snprintf(K.out + len, OUT_MAX - len, "]}\n");
            send_line(K.out, len);
        }
    }
    K.nprev = 0;
    for (UBaseType_t i = 0; i < n && K.nprev < MAX_TASKS; i++) {
        K.prev[K.nprev].num = K.tasks[i].xTaskNumber;
        K.prev[K.nprev].run = K.tasks[i].ulRunTimeCounter;
        K.nprev++;
    }
    K.tasks_prev_us = now;
#endif
}

/* The controller's signal, from the C6 (an RPC over SDIO): every 2 s at most. */
static void wifi_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (now - K.wifi_at < 2000000) return;
    K.wifi_at = now;
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        K.rssi_ok = false;
        return;
    }
    K.stations = list.num;
    K.rssi_ok = list.num > 0;
    int best = -127;
    for (int i = 0; i < list.num; i++) {
        if (list.sta[i].rssi > best) best = list.sta[i].rssi;
    }
    K.rssi = best;
}

static void send_stats(void)
{
    stadia_stats_t s;
    stadia_get_stats(&s);
    usb_pad_audio_stats_t u;
    usb_pad_audio_stats(&u);
    char rssi[12] = "null", bat[12] = "null", ping[2][12] = {"null", "null"};
    if (K.rssi_ok) snprintf(rssi, sizeof(rssi), "%d", K.rssi);
    if (s.battery_pct >= 0) snprintf(bat, sizeof(bat), "%d", s.battery_pct);
    for (int i = 0; i < 2; i++) {
        if (s.ping_us[i] >= 0) snprintf(ping[i], sizeof(ping[i]), "%d", (int)s.ping_us[i]);
    }
    char players[1024];
    players_json(players, sizeof(players));
    sendf("{\"t\":\"stats\",\"up\":%lld,\"players\":%s,\"cpu\":[%d,%d],\"heap\":[%u,%u,%u],"
          "\"ctl\":{\"state\":\"%s\",\"disp\":%s,\"game\":%s,\"rssi\":%s,\"sta\":%d,"
          "\"bat\":%s,\"bat_age\":%d,\"ping\":[%s,%s],\"pings\":[%u,%u],\"in\":[%u,%u,%u]},"
          "\"audio\":{\"route\":%s,\"cplx\":%u,\"mcplx\":%u,\"cplx_now\":%u,\"kbps\":%u,\"enc\":%u,\"sent\":%u,\"lost\":%u,\"trim\":%u,\"under\":%u,"
          "\"resync\":%u,\"bytes\":%llu,\"enc_us\":%llu,\"enc_max\":%u},"
          "\"mic\":{\"pkts\":%u,\"muted\":%u,\"dec\":%u,\"lost\":%u,\"err\":%u,\"drop\":%u,\"peak\":%d,\"age\":%d,"
          "\"talk\":%s},"
          "\"usb\":{\"mode\":%d,\"mounted\":%s,\"play\":%s,\"rec\":%s,\"vol\":[%d,%d],\"mute\":[%s,%s],\"pkts\":%u,"
          "\"fail\":%u,\"ovf\":%u,\"miss\":%u,\"mpkts\":%u,\"munder\":%u,\"mskip\":%u,\"mdrop\":%u,\"mbuf\":%u,\"cdrop\":%u}}",
          (long long)(esp_timer_get_time() / 1000), players, K.cpu[0], K.cpu[1],
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), s.display_state ? s.display_state : "none",
          s.display_up ? "true" : "false", s.game_up ? "true" : "false", rssi, K.stations,
          bat, (int)s.battery_age_s, ping[0], ping[1], (unsigned)s.pings_sent, (unsigned)s.pings_answered,
          (unsigned)s.input_series, (unsigned)s.input_gaps, (unsigned)s.input_repeats,
          s.audio_route ? "true" : "false", s.audio_complexity, s.audio_mic_complexity, s.audio_complexity_now, (unsigned)s.audio_kbps, (unsigned)s.audio_encoded, (unsigned)s.audio_sent,
          (unsigned)s.audio_not_sent, (unsigned)s.audio_trimmed, (unsigned)s.audio_underruns,
          (unsigned)s.audio_resyncs, (unsigned long long)s.audio_bytes, (unsigned long long)s.audio_enc_us,
          (unsigned)s.audio_enc_us_max, (unsigned)s.mic_packets, (unsigned)s.mic_muted, (unsigned)s.mic_decoded,
          (unsigned)s.mic_lost, (unsigned)s.mic_errors, (unsigned)s.mic_dropped, s.mic_peak, (int)s.mic_age_ms,
          s.mic_talking ? "true" : "false", (int)usb_pad_mode(), usb_pad_mounted() ? "true" : "false",
          u.playing ? "true" : "false", u.recording ? "true" : "false", u.volume[0], u.volume[1],
          u.mute[0] ? "true" : "false", u.mute[1] ? "true" : "false", (unsigned)u.packets, (unsigned)u.failed,
          (unsigned)u.overflows, (unsigned)u.missed, (unsigned)u.mic_packets, (unsigned)u.mic_underruns, (unsigned)u.mic_skips,
          (unsigned)u.mic_dropped, (unsigned)u.mic_buffered, (unsigned)usb_console_dropped());
}

/* ---- commands ------------------------------------------------------------------------ */

static void reply_err(const char *what)
{
    sendf("{\"t\":\"err\",\"msg\":\"%s\"}", what);
}

static void cmd_set(const char *key, const char *val)
{
    if (!key || !val) { reply_err("usage: set <key> <value>"); return; }
    char *end;
    long v = strtol(val, &end, 10);
    if (end == val) { reply_err("not a number"); return; }
    if (!strcmp(key, "mode")) {
        if (v < 0 || v >= USB_PAD_MODE_COUNT) { reply_err("mode is 0-3"); return; }
        sendf("{\"t\":\"ok\",\"key\":\"mode\",\"reconnect\":%s}", (usb_pad_mode_t)v != usb_pad_mode() ? "true" : "false");
        vTaskDelay(pdMS_TO_TICKS(50)); /* let the answer leave before the board leaves the bus */
        if (!pad_mode_request((usb_pad_mode_t)v)) { reply_err("busy changing modes"); return; }
        return;
    } else if (!strcmp(key, "players")) {
        if (v < 1 || v > STADIA_MAX_CONTROLLERS) { reply_err("players is 1-4"); return; }
        K.players = (int)v;
        players_save_count(K.players); /* from the next start: the USB gamepads and the links are set up once */
        sendf("{\"t\":\"ok\",\"key\":\"players\",\"restart\":%s}", K.players != K.players_running ? "true" : "false");
        send_settings();
        return;
    } else if (!strcmp(key, "gain")) {
        if (v < -12 || v > 0) { reply_err("gain is -12..0 dB"); return; }
        K.set.gain_db = (int8_t)v;
        apply_gain((int)v);
    } else if (!strcmp(key, "complexity")) {
        if (v < 1 || v > 10) { reply_err("complexity is 1-10"); return; }
        K.set.complexity = (uint8_t)v;
        stadia_audio_set_complexity((int)v);
    } else if (!strcmp(key, "mic_complexity")) {
        if (v < 1 || v > 10) { reply_err("mic_complexity is 1-10"); return; }
        K.set.mic_complexity = (uint8_t)v;
        stadia_audio_set_mic_complexity((int)v);
    } else if (!strcmp(key, "bitrate")) {
        if (v < STADIA_AUDIO_KBPS_MIN || v > STADIA_AUDIO_KBPS_MAX) { reply_err("bitrate is 32-160 kbps"); return; }
        K.set.kbps = (uint8_t)v;
        stadia_audio_set_bitrate((int)v);
    } else if (!strcmp(key, "mic")) {
#if !CONFIG_DONGLE_USB_MIC
        if (v) { reply_err("this firmware is built without the microphone"); return; }
#endif
        K.set.mic = v != 0; /* takes effect at the next start */
    } else if (!strcmp(key, "ptt")) {
        K.set.ptt = v != 0;
        stadia_mic_set_push_to_talk(K.set.ptt);
    } else if (!strcmp(key, "rumble")) {
        K.set.rumble = v != 0;
        apply_rumble();
    } else if (!strcmp(key, "rumble_left") || !strcmp(key, "rumble_right")) {
        if (v < 0 || v > RUMBLE_PCT_MAX) { reply_err("rumble strength is 0-200 %"); return; }
        K.set.rumble_pct[key[7] == 'r'] = (uint8_t)v;
        apply_rumble();
    } else {
        { reply_err("unknown setting"); return; }
    }
    settings_save();
    sendf("{\"t\":\"ok\",\"key\":\"%s\"}", key);
    send_settings();
}

/* rumble <left> <right> [ms]: a test through the rumble settings */
static void cmd_rumble(const char *l, const char *r, const char *ms)
{
    if (!l || !r) { reply_err("usage: rumble <left 0-255> <right 0-255> [ms]"); return; }
    long lv = strtol(l, NULL, 10), rv = strtol(r, NULL, 10), t = ms ? strtol(ms, NULL, 10) : 1000;
    if (lv < 0 || lv > 255 || rv < 0 || rv > 255) { reply_err("rumble levels are 0-255"); return; }
    if (t < 50 || t > RUMBLE_TEST_MS_MAX) { reply_err("rumble test is 50-5000 ms"); return; }
    const char *err = rumble_test((uint8_t)lv, (uint8_t)rv, (int)t);
    if (err) { reply_err(err); return; }
    sendf("{\"t\":\"ok\",\"key\":\"rumble\"}");
}

/* the pairing's progress, when it changes */
static void send_pair(bool always)
{
    static uint32_t gen;
    pair_status_t p;
    pair_get_status(&p);
    if (!always && p.gen == gen) return;
    gen = p.gen;
    char msg[200];
    size_t j = 0;
    for (const char *c = p.msg; *c && j < sizeof(msg) - 2; c++) { /* JSON-safe */
        if (*c == '"' || *c == '\\') msg[j++] = '\\';
        msg[j++] = *c;
    }
    msg[j] = 0;
    sendf("{\"t\":\"pair\",\"state\":\"%s\",\"busy\":%s,\"msg\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"setup\":%d}", p.state,
          p.busy ? "true" : "false", msg, p.name, p.rssi, p.setup_state);
}

/* the motors' levels, when they change (at most every 50 ms, with the pad) */
static void send_rumble(void)
{
    uint16_t out[2];
    uint8_t host[2];
    bool testing;
    if (!rumble_levels(out, host, &testing)) return;
    sendf("{\"t\":\"rumble\",\"out\":[%u,%u],\"host\":[%u,%u],\"test\":%s}", out[0], out[1], host[0], host[1],
          testing ? "true" : "false");
}

static void handle(char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) return;
    if (!strcmp(cmd, "hello")) {
        send_info();
        send_settings();
        ota_send_status(sendf);
        send_pair(true);
    } else if (!strcmp(cmd, "tasks")) {
        K.tasks_req = true; /* the console task lists them with the next stats */
    } else if (!strcmp(cmd, "get")) {
        send_settings();
    } else if (!strcmp(cmd, "set")) {
        char *key = strtok_r(NULL, " \t", &save);
        char *val = strtok_r(NULL, " \t", &save);
        cmd_set(key, val);
    } else if (!strcmp(cmd, "defaults")) {
        bool running = K.set.mic_running;
        defaults(&K.set);
        K.set.mic_running = running;
        apply_gain(K.set.gain_db);
        stadia_audio_set_complexity(K.set.complexity);
        stadia_audio_set_mic_complexity(K.set.mic_complexity);
        stadia_audio_set_bitrate(K.set.kbps);
        stadia_mic_set_push_to_talk(K.set.ptt);
        apply_rumble();
        settings_save();
        sendf("{\"t\":\"ok\",\"key\":\"defaults\"}");
        send_settings();
    } else if (!strcmp(cmd, "logs")) {
        char *v = strtok_r(NULL, " \t", &save);
        K.logs = v && (!strcmp(v, "on") || !strcmp(v, "1"));
        sendf("{\"t\":\"ok\",\"key\":\"logs\"}");
        send_settings();
    } else if (!strcmp(cmd, "rumble")) {
        cmd_rumble(strtok_r(NULL, " \t", &save), strtok_r(NULL, " \t", &save), strtok_r(NULL, " \t", &save));
    } else if (!strcmp(cmd, "ota")) {
        static char none[1];
        ota_run(save ? save : none, sendf);
    } else if (!strcmp(cmd, "forget")) {
        char *v = strtok_r(NULL, " \t", &save);
        long n = v ? strtol(v, NULL, 10) : 0;
        if (n < 1 || n > K.players_running) { reply_err("forget <player>: a player number of this start"); return; }
        stadia_forget_player((int)n - 1);
        sendf("{\"t\":\"ok\",\"key\":\"forget\"}");
    } else if (!strcmp(cmd, "pair")) {
        char *v = strtok_r(NULL, " \t", &save);
        if (v && !strcmp(v, "stop")) {
            pair_stop();
            sendf("{\"t\":\"ok\",\"key\":\"pair\"}");
            return;
        }
        const char *err = pair_start();
        if (err) { reply_err(err); return; }
        sendf("{\"t\":\"ok\",\"key\":\"pair\"}");
    } else if (!strcmp(cmd, "reboot")) {
        sendf("{\"t\":\"ok\",\"key\":\"reboot\",\"reconnect\":true}");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        reply_err("unknown command");
    }
}

static void console_task(void *arg)
{
    char *line = malloc(USB_CONSOLE_LINE_MAX);
    if (!line) vTaskDelete(NULL);
    int64_t next_stats = 0;
    int tick = 0;
    for (;;) {
        if (usb_console_read_line(line, 50)) handle(line);
        if (!usb_console_active()) {
            next_stats = 0;
            K.prev_us = K.tasks_prev_us = 0; /* the next CPU figures start fresh */
            if (K.logs) K.logs = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        send_pad();
        send_rumble();
        send_pair(false);
        int64_t now = esp_timer_get_time();
        if (now >= next_stats) {
            next_stats = now + 1000000;
            wifi_poll();
            cpu_cores();
            usb_pad_audio_stats_t u;
            usb_pad_audio_stats(&u);
            /* on "tasks" with nothing to measure from: a start now, the list 2 s later */
            if (K.tasks_req && !K.tasks_prev_us) K.tasks_again = 2;
            if ((tick++ % 2 == 1 && !u.playing) || K.tasks_req || (K.tasks_again && !--K.tasks_again)) {
                K.tasks_req = false;
                tasks_snapshot();
            }
            send_stats();
        }
    }
}

void console_start(bool mic_running, int players)
{
    K.set.mic_running = mic_running;
    K.players = K.players_running = players;
    K.out = malloc(OUT_MAX);
    K.log_buf = malloc(LOG_LINE);
    K.log_lock = xSemaphoreCreateMutex();
#if CONSOLE_CPU
    K.tasks = malloc(sizeof(TaskStatus_t) * MAX_TASKS);
    K.prev = malloc(sizeof(*K.prev) * MAX_TASKS);
    if (!K.prev) K.tasks = NULL;
#endif
    if (!K.out || !K.log_lock) {
        ESP_LOGE(TAG, "no memory for the console");
        return;
    }
    pair_init();
    K.started = true;
    K.prev_vprintf = esp_log_set_vprintf(log_vprintf);
    /* low priority: stats wait for the audio */
    if (xTaskCreate(console_task, "console", 6144, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the console task");
        return;
    }
    ESP_LOGI(TAG, "console on the USB serial port (web console: Chrome or Edge, see firmware/esp32p4/console)");
}
