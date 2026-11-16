/* Run the Stadiacast core on Linux: stadia_host [ip] [https-port] [rtc-port] [game-port]
 * STADIA_ALLOW_CLEARCUT=1 answers Clearcut instead of refusing it, as the fake controller expects.
 * STADIA_START_GAME=1 sends StartGame once READY (run the fake controller with --game).
 * Rumble test: pressing A starts both motors; 6 s after A is released they stop, so a held
 * level outlives the controller's 5 s haptics watchdog only through our refreshes.
 * STADIA_AUDIO=1 opens the headset audio route and plays a 440 Hz tone into it, 10 ms at a time
 * as a USB host would (the fake controller counts the frames it would play).
 * STADIA_AUDIO_KBPS=48 sets the Opus bitrate (default 96).
 * STADIA_MIC=1 also decodes what the controller sends as its microphone (run the fake
 * controller with --mic-echo: it sends the tone back) and prints the decoded tone's frequency.
 * STADIA_MIC_PTT=1 passes it only while Assistant is held (fake controller --talk SECONDS).
 * STADIA_STATS=1 prints the battery and link quality of stadia_get_stats() every second. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "stadia.h"

static atomic_llong s_stop_at; /* ms, 0 = none */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void on_pad(const stadia_pad_t *pad, void *arg)
{
    static bool a_was;
    bool a = pad->connected && (pad->buttons & STADIA_BTN_A);
    if (a && !a_was) {
        atomic_store(&s_stop_at, 0);
        stadia_rumble(40000, 20000);
    } else if (!a && a_was) {
        atomic_store(&s_stop_at, now_ms() + 6000);
    }
    a_was = a;
}

static void *tone_thread(void *arg)
{
    static int16_t pcm[480 * 2];
    double phase = 0;
    long long next = now_ms();
    for (;;) {
        for (int i = 0; i < 480; i++) {
            int16_t v = (int16_t)(8000 * sin(phase));
            pcm[2 * i] = pcm[2 * i + 1] = v;
            phase += 2 * M_PI * 440 / 48000;
        }
        stadia_audio_pcm(pcm, 480);
        next += 10;
        long long wait = next - now_ms();
        if (wait > 0) usleep((useconds_t)wait * 1000);
    }
    return NULL;
}

/* The decoded microphone: count zero crossings per second, which for the echoed 440 Hz tone
 * should be about 880. */
static void on_mic(const int16_t *pcm, size_t frames, void *arg)
{
    static long long samples, crossings, peak;
    static int16_t prev;
    for (size_t i = 0; i < frames; i++) {
        if ((prev < 0) != (pcm[i] < 0)) crossings++;
        prev = pcm[i];
        long long a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    samples += (long long)frames;
    if (samples >= 48000 * 5) {
        fprintf(stderr, "stadia_host: microphone %.1f s, %.0f Hz, peak %lld\n", samples / 48000.0,
                crossings / 2.0 / (samples / 48000.0), peak);
        samples = crossings = peak = 0;
    }
}

int main(int argc, char **argv)
{
    uint16_t rtc_port = (uint16_t)(argc > 3 ? atoi(argv[3]) : 40000);
    stadia_config_t cfg = {
        .advertise_ip = argc > 1 ? argv[1] : "127.0.0.1",
        .https_port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 8443),
        .rtc_port = rtc_port,
        .game_port = (uint16_t)(argc > 4 ? atoi(argv[4]) : rtc_port + 1),
        .start_game = getenv("STADIA_START_GAME") != NULL,
        .allow_clearcut = getenv("STADIA_ALLOW_CLEARCUT") != NULL,
        .audio = getenv("STADIA_AUDIO") != NULL,
        .audio_kbps = getenv("STADIA_AUDIO_KBPS") ? (uint8_t)atoi(getenv("STADIA_AUDIO_KBPS")) : 0,
        .mic = getenv("STADIA_MIC") != NULL,
        .mic_push_to_talk = getenv("STADIA_MIC_PTT") != NULL,
    };
    stadia_set_pad_callback(on_pad, NULL);
    stadia_set_mic_callback(on_mic, NULL);
    if (!stadia_start(&cfg)) {
        return 1;
    }
    if (cfg.audio) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, STADIA_AUDIO_STACK);
        pthread_t t;
        pthread_create(&t, &attr, tone_thread, NULL);
    }
    fprintf(stderr, "stadia_host: ready\n");
    bool stats = getenv("STADIA_STATS") != NULL;
    long long next_stats = now_ms() + 1000;
    for (;;) {
        usleep(50 * 1000);
        if (stats && now_ms() >= next_stats) {
            next_stats += 1000;
            stadia_stats_t st;
            stadia_get_stats(&st);
            fprintf(stderr, "stadia_host: stats state=%s battery=%d%% age=%ds ping_us=%d/%d pings=%u/%u input=%u gaps=%u repeats=%u\n",
                    st.display_state, st.battery_pct, (int)st.battery_age_s, (int)st.ping_us[0], (int)st.ping_us[1],
                    (unsigned)st.pings_answered, (unsigned)st.pings_sent, (unsigned)st.input_series,
                    (unsigned)st.input_gaps, (unsigned)st.input_repeats);
        }
        long long stop = atomic_load(&s_stop_at);
        if (stop && now_ms() >= stop) {
            atomic_store(&s_stop_at, 0);
            stadia_rumble(0, 0);
        }
    }
}
