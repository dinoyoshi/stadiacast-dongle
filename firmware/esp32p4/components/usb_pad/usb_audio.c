/*
 * USB Audio Class 2.0 speaker as a TinyUSB application class driver (esp_tinyusb leaves the
 * built-in audio class off, and a small driver of our own is easier to keep beside XInput).
 *
 * The host streams 48 kHz 16-bit stereo on isochronous OUT 0x02, one packet per millisecond.
 * The packets are taken in the USB interrupt (xfer_isr), so the endpoint is re-armed before the
 * next one arrives whatever the USB task is doing, and go into a stream buffer that
 * usb_pad_audio_read() drains. The clock is fixed at 48 kHz; volume (-50..0 dB) and mute are
 * applied when the samples are read.
 */
#include "usb_audio.h"
#include "usb_pad.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "usb_audio";

#define RATE 48000
#define VOL_MIN (-50 * 256) /* 1/256 dB */
#define VOL_MAX 0
#define VOL_RES 256
#define RING_BYTES (48 * 4 * 60) /* 60 ms */

typedef struct {
    TUD_EPBUF_DEF(out, USB_AUDIO_EP_SIZE);
} uac_epbuf_t;

CFG_TUD_MEM_SECTION static uac_epbuf_t s_buf;

static StaticStreamBuffer_t s_ring_ctl;
static uint8_t s_ring_mem[RING_BYTES + 1];

static struct {
    StreamBufferHandle_t ring;
    uint8_t rhport;
    uint8_t ac_itf, as_itf, alt;
    const tusb_desc_endpoint_t *ep;
    uint8_t ctrl[16];
    bool mute;
    int16_t volume;     /* 1/256 dB */
    volatile int32_t gain; /* Q15, 0 when muted */
    volatile uint32_t overflows;
} U;

static void update_gain(void)
{
    U.gain = U.mute ? 0 : (int32_t)lroundf(32768.0f * powf(10.0f, U.volume / 256.0f / 20.0f));
}

bool usb_audio_init(void)
{
    U.ring = xStreamBufferCreateStatic(RING_BYTES, 4, s_ring_mem, &s_ring_ctl);
    U.volume = 0;
    update_gain();
    return U.ring != NULL;
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
    U.alt = 0;
    U.ep = NULL;
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
    while (len < max_len) {
        if (len && tu_desc_type(p) == TUSB_DESC_INTERFACE_ASSOCIATION) break;
        if (tu_desc_type(p) == TUSB_DESC_INTERFACE) {
            tusb_desc_interface_t const *d = (tusb_desc_interface_t const *)p;
            if (d->bInterfaceClass != TUSB_CLASS_AUDIO) break;
            if (d->bInterfaceSubClass == AUDIO_SUBCLASS_STREAMING) U.as_itf = d->bInterfaceNumber;
        } else if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            U.ep = (tusb_desc_endpoint_t const *)p;
            TU_ASSERT(usbd_edpt_iso_alloc(rhport, U.ep->bEndpointAddress, tu_edpt_packet_size(U.ep)), 0);
        }
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    TU_VERIFY(U.ep, 0);
    U.alt = 0;
    return len;
}

static bool set_interface(uint8_t rhport, uint8_t itf, uint8_t alt)
{
    if (itf != U.as_itf) return itf == U.ac_itf && alt == 0;
    if (alt > 1) return false;
    if (alt == 1) { /* (again) from a clean endpoint: activating drops a transfer still armed */
        if (U.alt != 1) ESP_LOGI(TAG, "the host started playing");
        U.alt = 1;
        TU_ASSERT(usbd_edpt_iso_activate(rhport, U.ep));
        TU_ASSERT(usbd_edpt_xfer(rhport, USB_AUDIO_EP_OUT, s_buf.out, USB_AUDIO_EP_SIZE, false));
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

    if (!get) { /* SET CUR: take the data stage, act on it */
        if (req->bRequest != AUDIO20_CS_REQ_CUR || req->wLength > sizeof(U.ctrl)) return false;
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, req, U.ctrl, req->wLength);
        if (stage != CONTROL_STAGE_DATA) return true;
        if (entity == UAC_FU && cn == 0 && cs == AUDIO20_FU_CTRL_MUTE && req->wLength >= 1) {
            U.mute = b[0] != 0;
            update_gain();
            ESP_LOGI(TAG, "mute %s", U.mute ? "on" : "off");
            return true;
        }
        if (entity == UAC_FU && cn == 0 && cs == AUDIO20_FU_CTRL_VOLUME && req->wLength >= 2) {
            int16_t v = (int16_t)(b[0] | b[1] << 8);
            U.volume = v < VOL_MIN ? VOL_MIN : v > VOL_MAX ? VOL_MAX : v;
            update_gain();
            ESP_LOGI(TAG, "volume %.1f dB", U.volume / 256.0);
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
    } else if (entity == UAC_FU && cn == 0 && cs == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
        b[0] = U.mute;
        n = 1;
    } else if (entity == UAC_FU && cn == 0 && cs == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_CUR) {
        tu_unaligned_write16(b, tu_htole16((uint16_t)U.volume));
        n = 2;
    } else if (entity == UAC_FU && cn == 0 && cs == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        tu_unaligned_write16(b, tu_htole16(1));
        tu_unaligned_write16(b + 2, tu_htole16((uint16_t)VOL_MIN));
        tu_unaligned_write16(b + 4, tu_htole16((uint16_t)VOL_MAX));
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
            U.ctrl[0] = itf == U.as_itf ? U.alt : 0;
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
    if (ep_addr != USB_AUDIO_EP_OUT) return false;
    BaseType_t woken = pdFALSE;
    n &= ~3u;
    if (result == XFER_RESULT_SUCCESS && n && U.alt == 1) {
        if (xStreamBufferSpacesAvailable(U.ring) >= n) {
            xStreamBufferSendFromISR(U.ring, s_buf.out, n, &woken);
        } else {
            U.overflows++;
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
    int32_t g = U.gain;
    if (g != 32768) {
        for (size_t i = 0; i < n * 2; i++) pcm[i] = (int16_t)((pcm[i] * g) >> 15);
    }
    return n;
}
