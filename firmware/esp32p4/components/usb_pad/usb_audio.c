/*
 * USB Audio Class 2.0 headset as a TinyUSB application class driver (esp_tinyusb leaves the
 * built-in audio class off, and a small driver of our own is easier to keep beside XInput).
 *
 * Headphones: the host streams 48 kHz 16-bit stereo on isochronous OUT 0x02, one packet per
 * (micro)frame. The packets are taken in the USB interrupt (xfer_isr), so the endpoint is
 * re-armed before the next one arrives whatever the USB task is doing, and go into a stream
 * buffer that usb_pad_audio_read() drains. Volume (-50..0 dB) and mute are applied when the
 * samples are read.
 *
 * Microphone: usb_pad_mic_write() fills a ring of mono samples; the interrupt sends 48 of them
 * per 1 ms frame (6 per 125 us microframe at high speed) on isochronous IN 0x82, arming the next
 * packet as each one completes. The ring starts MIC_PRIME_MS deep and follows the host's clock,
 * not the controller's: past MIC_MAX_MS of backlog it skips back to MIC_PRIME_MS, and when it
 * runs dry the host gets silence until it has primed again. Volume (-50..0 dB) and mute are
 * applied as the packet is built.
 *
 * The clock is fixed at 48 kHz for both.
 */
#include "usb_audio.h"
#include "usb_pad.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"

static const char *TAG = "usb_audio";

#define RATE 48000
#define VOL_MIN (-50 * 256) /* 1/256 dB */
#define VOL_MAX 0
#define VOL_RES 256
#define MIC_VOL_MAX 0 /* hosts put a new microphone at the top of its range: +20 dB clipped */
#define RING_BYTES (48 * 4 * 60) /* 60 ms */
#define MIC_RING 8192            /* samples, a power of two: 170 ms */
#define MIC_PRIME_MS 40
#define MIC_MAX_MS 120

enum { FU_SPK, FU_MIC };

typedef struct {
    TUD_EPBUF_DEF(out, USB_AUDIO_EP_SIZE);
    TUD_EPBUF_DEF(in, USB_MIC_EP_SIZE);
} uac_epbuf_t;

CFG_TUD_MEM_SECTION static uac_epbuf_t s_buf;

static struct {
    StreamBufferHandle_t ring;
    uint8_t rhport;
    uint8_t ac_itf, as_itf, alt;
    const tusb_desc_endpoint_t *ep;
    uint8_t ctrl[16];
    bool mute[2];
    int16_t volume[2];        /* 1/256 dB */
    volatile int32_t gain[2]; /* Q15, 0 when muted */
    volatile uint32_t overflows;
    volatile uint32_t packets, bytes, failed; /* since the last report */
    int64_t report_at;
    /* the microphone */
    uint8_t mic_itf, mic_alt;
    const tusb_desc_endpoint_t *mic_ep;
    int16_t *mic_ring;              /* MIC_RING samples */
    volatile uint32_t mic_head;     /* written by usb_pad_mic_write */
    volatile uint32_t mic_tail;     /* read by the interrupt */
    volatile bool mic_primed;
    uint16_t mic_per_packet;        /* 48 at full speed, 6 at high speed */
    volatile uint32_t mic_packets, mic_underruns, mic_skips, mic_dropped, mic_written;
    int64_t mic_report_at;
    /* the same, from power-up, for the console (usb_pad_audio_stats) */
    volatile uint32_t t_packets, t_failed, t_overflows, t_mic_packets, t_mic_underruns, t_mic_skips, t_mic_dropped;
} U;

static void update_gain(int fu)
{
    U.gain[fu] = U.mute[fu] ? 0 : (int32_t)lroundf(32768.0f * powf(10.0f, U.volume[fu] / 256.0f / 20.0f));
}

bool usb_audio_init(void)
{
    /* From the heap, not .bss: on v1.x P4 chips .bss eats into the small RAM pool the system
     * starts from (see c6_update.c). Internal RAM, as the USB ISR writes it. */
    U.ring = xStreamBufferCreate(RING_BYTES, 480 * 4); /* wake the reader per 10 ms */
    U.mic_ring = heap_caps_calloc(MIC_RING, sizeof(int16_t), MALLOC_CAP_INTERNAL);
    U.volume[FU_SPK] = U.volume[FU_MIC] = 0;
    update_gain(FU_SPK);
    update_gain(FU_MIC);
    return U.ring != NULL && U.mic_ring != NULL;
}

/* ---- the class driver --------------------------------------------------------------- */

static void uac_init(void)
{
}

static bool uac_deinit(void)
{
    return true;
}

static void uac_reset(uint8_t rhport)
{
    U.alt = U.mic_alt = 0;
    U.ep = U.mic_ep = NULL;
}

/* The audio function: the AC interface, then the AS interface's alternate settings. */
static uint16_t uac_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != TUSB_CLASS_AUDIO || itf->bInterfaceSubClass != AUDIO_SUBCLASS_CONTROL ||
        itf->bInterfaceProtocol != AUDIO_INT_PROTOCOL_CODE_V2) {
        return 0;
    }
    U.rhport = rhport;
    U.ac_itf = itf->bInterfaceNumber;
    uint16_t len = 0;
    uint8_t const *p = (uint8_t const *)itf;
    uint8_t cur = 0;
    U.ep = U.mic_ep = NULL;
    while (len < max_len) {
        if (len && tu_desc_type(p) == TUSB_DESC_INTERFACE_ASSOCIATION) break;
        if (tu_desc_type(p) == TUSB_DESC_INTERFACE) {
            tusb_desc_interface_t const *d = (tusb_desc_interface_t const *)p;
            if (d->bInterfaceClass != TUSB_CLASS_AUDIO) break;
            cur = d->bInterfaceNumber;
        } else if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            /* each AS interface's alt 1 has one endpoint: OUT the headphones, IN the microphone */
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                U.mic_ep = ep;
                U.mic_itf = cur;
            } else {
                U.ep = ep;
                U.as_itf = cur;
            }
            TU_ASSERT(usbd_edpt_iso_alloc(rhport, ep->bEndpointAddress, tu_edpt_packet_size(ep)), 0);
        }
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    TU_VERIFY(U.ep, 0);
    U.alt = U.mic_alt = 0;
    return len;
}

/* The next microphone packet into s_buf.in: mic_per_packet samples from the ring, or silence
 * while it primes. Runs in the interrupt (and once from set_interface). */
static uint16_t mic_packet(void)
{
    int16_t *out = (int16_t *)s_buf.in;
    uint32_t want = U.mic_per_packet, head = U.mic_head, tail = U.mic_tail;
    uint32_t avail = head - tail;
    if (avail > MIC_MAX_MS * 48) { /* a backlog: the controller's clock ran ahead of the host's */
        tail = head - MIC_PRIME_MS * 48;
        avail = MIC_PRIME_MS * 48;
        U.mic_skips++;
        U.t_mic_skips++;
    }
    if (!U.mic_primed && avail >= MIC_PRIME_MS * 48) U.mic_primed = true;
    if (U.mic_primed && avail < want) {
        U.mic_primed = false;
        U.mic_underruns++;
        U.t_mic_underruns++;
    }
    int32_t g = U.gain[FU_MIC];
    for (uint32_t i = 0; i < want; i++) {
        if (!U.mic_primed) {
            out[i] = 0;
            continue;
        }
        int32_t v = (U.mic_ring[tail++ & (MIC_RING - 1)] * g) >> 15;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    U.mic_tail = tail;
    U.mic_packets++;
    U.t_mic_packets++;
    return (uint16_t)(want * 2);
}

static bool mic_set_interface(uint8_t rhport, uint8_t alt)
{
    if (alt > 1) return false;
    if (alt == 1) {
        if (U.mic_alt != 1) ESP_LOGI(TAG, "the host started recording");
        U.mic_alt = 1;
        U.mic_per_packet = tud_speed_get() == TUSB_SPEED_HIGH ? 6 : 48;
        U.mic_tail = U.mic_head; /* nothing stale: prime from what comes next */
        U.mic_primed = false;
        TU_ASSERT(usbd_edpt_iso_activate(rhport, U.mic_ep));
        if (!usbd_edpt_xfer(rhport, USB_AUDIO_EP_IN, s_buf.in, mic_packet(), false)) {
            ESP_LOGE(TAG, "could not arm the microphone endpoint");
            return false;
        }
    } else if (U.mic_alt == 1) {
        ESP_LOGI(TAG, "the host stopped recording (%u underruns, %u skips)", (unsigned)U.mic_underruns,
                 (unsigned)U.mic_skips);
        U.mic_alt = 0;
    }
    return true;
}

static bool set_interface(uint8_t rhport, uint8_t itf, uint8_t alt)
{
    if (U.mic_ep && itf == U.mic_itf) return mic_set_interface(rhport, alt);
    if (itf != U.as_itf) return itf == U.ac_itf && alt == 0;
    if (alt > 1) return false;
    if (alt == 1) { /* (again) from a clean endpoint: activating drops a transfer still armed */
        if (U.alt != 1) ESP_LOGI(TAG, "the host started playing");
        U.alt = 1;
        TU_ASSERT(usbd_edpt_iso_activate(rhport, U.ep));
        if (!usbd_edpt_xfer(rhport, USB_AUDIO_EP_OUT, s_buf.out, USB_AUDIO_EP_SIZE, false)) {
            ESP_LOGE(TAG, "could not arm the audio endpoint");
            return false;
        }
    } else if (U.alt == 1) {
        ESP_LOGI(TAG, "the host stopped playing (%u packets did not fit)", (unsigned)U.overflows);
        U.overflows = 0;
        U.alt = 0;
    }
    return true;
}

/* GET/SET CUR and GET RANGE on the clock and the feature unit. */
static bool entity_request(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    uint8_t entity = tu_u16_high(req->wIndex), cs = tu_u16_high(req->wValue), cn = tu_u16_low(req->wValue);
    bool get = req->bmRequestType_bit.direction == TUSB_DIR_IN;
    uint8_t *b = U.ctrl;
    static const char *const FU_NAME[] = {"headphones", "microphone"};
    int fu = entity == UAC_FU ? FU_SPK : entity == UAC_MIC_FU ? FU_MIC : -1;
    int16_t vol_max = fu == FU_MIC ? MIC_VOL_MAX : VOL_MAX;

    if (!get) { /* SET CUR: take the data stage, act on it */
        if (req->bRequest != AUDIO20_CS_REQ_CUR || req->wLength > sizeof(U.ctrl)) return false;
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, req, U.ctrl, req->wLength);
        if (stage != CONTROL_STAGE_DATA) return true;
        if (fu >= 0 && cn == 0 && cs == AUDIO20_FU_CTRL_MUTE && req->wLength >= 1) {
            U.mute[fu] = b[0] != 0;
            update_gain(fu);
            ESP_LOGI(TAG, "%s mute %s", FU_NAME[fu], U.mute[fu] ? "on" : "off");
            return true;
        }
        if (fu >= 0 && cn == 0 && cs == AUDIO20_FU_CTRL_VOLUME && req->wLength >= 2) {
            int16_t v = (int16_t)(b[0] | b[1] << 8);
            U.volume[fu] = v < VOL_MIN ? VOL_MIN : v > vol_max ? vol_max : v;
            update_gain(fu);
            ESP_LOGI(TAG, "%s volume %.1f dB", FU_NAME[fu], U.volume[fu] / 256.0);
            return true;
        }
        if (entity == UAC_CLK && cs == AUDIO20_CS_CTRL_SAM_FREQ && req->wLength >= 4) {
            return tu_le32toh(tu_unaligned_read32(b)) == RATE;
        }
        return false;
    }

    if (stage != CONTROL_STAGE_SETUP) return true;
    uint16_t n = 0;
    if (entity == UAC_CLK && cs == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_CUR) {
        tu_unaligned_write32(b, tu_htole32(RATE));
        n = 4;
    } else if (entity == UAC_CLK && cs == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        tu_unaligned_write16(b, tu_htole16(1)); /* one sub-range: 48000..48000 step 0 */
        tu_unaligned_write32(b + 2, tu_htole32(RATE));
        tu_unaligned_write32(b + 6, tu_htole32(RATE));
        tu_unaligned_write32(b + 10, 0);
        n = 14;
    } else if (entity == UAC_CLK && cs == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
        b[0] = 1;
        n = 1;
    } else if (fu >= 0 && cn == 0 && cs == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
        b[0] = U.mute[fu];
        n = 1;
    } else if (fu >= 0 && cn == 0 && cs == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_CUR) {
        tu_unaligned_write16(b, tu_htole16((uint16_t)U.volume[fu]));
        n = 2;
    } else if (fu >= 0 && cn == 0 && cs == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        tu_unaligned_write16(b, tu_htole16(1));
        tu_unaligned_write16(b + 2, tu_htole16((uint16_t)VOL_MIN));
        tu_unaligned_write16(b + 4, tu_htole16((uint16_t)vol_max));
        tu_unaligned_write16(b + 6, tu_htole16(VOL_RES));
        n = 8;
    } else {
        return false; /* stall what we do not have */
    }
    return tud_control_xfer(rhport, req, b, n);
}

static bool uac_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        uint8_t itf = tu_u16_low(req->wIndex);
        if (req->bRequest == TUSB_REQ_SET_INTERFACE) {
            if (!set_interface(rhport, itf, tu_u16_low(req->wValue))) return false;
            return tud_control_status(rhport, req);
        }
        if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
            U.ctrl[0] = itf == U.as_itf ? U.alt : U.mic_ep && itf == U.mic_itf ? U.mic_alt : 0;
            return tud_control_xfer(rhport, req, U.ctrl, 1);
        }
        return false;
    }
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS && tu_u16_low(req->wIndex) == U.ac_itf) {
        return entity_request(rhport, stage, req);
    }
    return false;
}

/* In the USB interrupt: keep the samples, re-arm at once. */
static bool uac_xfer_isr(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n)
{
    if (ep_addr == USB_AUDIO_EP_IN) { /* a microphone packet went: the next one */
        if (U.mic_alt == 1) usbd_edpt_xfer(rhport, USB_AUDIO_EP_IN, s_buf.in, mic_packet(), true);
        return true;
    }
    if (ep_addr != USB_AUDIO_EP_OUT) return false;
    BaseType_t woken = pdFALSE;
    n &= ~3u;
    if (result != XFER_RESULT_SUCCESS) U.failed++, U.t_failed++;
    U.packets++;
    U.t_packets++;
    U.bytes += n;
    if (result == XFER_RESULT_SUCCESS && n && U.alt == 1) {
        if (xStreamBufferSpacesAvailable(U.ring) >= n) {
            xStreamBufferSendFromISR(U.ring, s_buf.out, n, &woken);
        } else {
            U.overflows++;
            U.t_overflows++;
        }
    }
    if (U.alt == 1) usbd_edpt_xfer(rhport, USB_AUDIO_EP_OUT, s_buf.out, USB_AUDIO_EP_SIZE, true);
    portYIELD_FROM_ISR(woken);
    return true;
}

static bool uac_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n)
{
    return true;
}

const usbd_class_driver_t usb_audio_driver = {
    .name = "UAC2",
    .init = uac_init,
    .deinit = uac_deinit,
    .reset = uac_reset,
    .open = uac_open,
    .control_xfer_cb = uac_control_xfer_cb,
    .xfer_cb = uac_xfer_cb,
    .xfer_isr = uac_xfer_isr,
    .sof = NULL,
};

/* ---- API ---------------------------------------------------------------------------- */

size_t usb_pad_audio_read(int16_t *pcm, size_t max_frames, uint32_t timeout_ms)
{
    if (!U.ring) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return 0;
    }
    size_t n = xStreamBufferReceive(U.ring, pcm, max_frames * 4, pdMS_TO_TICKS(timeout_ms)) / 4;
    int64_t now = esp_timer_get_time();
    if (now - U.report_at >= 10000000) { /* what the endpoint did in the last 10 s, while playing */
        uint32_t packets = U.packets, bytes = U.bytes, failed = U.failed;
        U.packets = U.bytes = U.failed = 0;
        if (U.alt == 1 || packets) {
            ESP_LOGI(TAG, "%u packets (%u B, %u failed) in %d s, %u did not fit", (unsigned)packets, (unsigned)bytes,
                     (unsigned)failed, (int)((now - U.report_at) / 1000000), (unsigned)U.overflows);
        }
        U.report_at = now;
    }
    int32_t g = U.gain[FU_SPK];
    if (g != 32768) {
        for (size_t i = 0; i < n * 2; i++) pcm[i] = (int16_t)((pcm[i] * g) >> 15);
    }
    return n;
}

void usb_pad_mic_write(const int16_t *pcm, size_t frames)
{
    if (!U.mic_ring) return;
    uint32_t head = U.mic_head;
    uint32_t space = MIC_RING - 1 - (head - U.mic_tail);
    if (frames > space) { /* nobody is reading (the host is not recording): keep the newest */
        U.mic_dropped += (uint32_t)(frames - space);
        U.t_mic_dropped += (uint32_t)(frames - space);
        if (U.mic_alt != 1) {
            U.mic_tail = head; /* the reader is idle: the writer may move it */
            space = MIC_RING - 1;
        }
        if (frames > space) frames = space;
    }
    for (size_t i = 0; i < frames; i++) U.mic_ring[(head + i) & (MIC_RING - 1)] = pcm[i];
    U.mic_head = head + (uint32_t)frames; /* published after the samples */
    U.mic_written += (uint32_t)frames;

    int64_t now = esp_timer_get_time();
    if (now - U.mic_report_at >= 10000000) {
        if (U.mic_alt == 1) {
            ESP_LOGI(TAG, "microphone: %u ms in, %u packets out in %d s, %u underruns, %u skips", (unsigned)(U.mic_written / 48),
                     (unsigned)U.mic_packets, (int)((now - U.mic_report_at) / 1000000), (unsigned)U.mic_underruns,
                     (unsigned)U.mic_skips);
        }
        U.mic_written = U.mic_packets = U.mic_underruns = U.mic_skips = 0;
        U.mic_report_at = now;
    }
}

void usb_pad_audio_stats(usb_pad_audio_stats_t *st)
{
    *st = (usb_pad_audio_stats_t){
        .playing = U.alt == 1,
        .recording = U.mic_alt == 1,
        .mute = {U.mute[FU_SPK], U.mute[FU_MIC]},
        .volume = {U.volume[FU_SPK], U.volume[FU_MIC]},
        .packets = U.t_packets,
        .failed = U.t_failed,
        .overflows = U.t_overflows,
        .mic_packets = U.t_mic_packets,
        .mic_underruns = U.t_mic_underruns,
        .mic_skips = U.t_mic_skips,
        .mic_dropped = U.t_mic_dropped,
        .mic_buffered = U.mic_head - U.mic_tail,
    };
}
