/*
 * The USB Audio Class 2.0 speaker next to the XInput interface (usb_audio.c): one 48 kHz,
 * 16-bit stereo OUT stream (adaptive, no feedback endpoint), with the host's volume and mute.
 */
#pragma once

#include "tinyusb.h"
#include "device/usbd_pvt.h"
#include "class/audio/audio.h"

#define USB_AUDIO_EP_OUT 0x02
#define USB_AUDIO_EP_SIZE 196 /* 49 frames of 4 bytes: one 1 ms packet with room for a spare frame */
#define USB_AUDIO_STRING 4    /* "Controller headset" */

/* entities */
#define UAC_IT 0x01
#define UAC_FU 0x02
#define UAC_OT 0x03
#define UAC_CLK 0x04

#define UAC_STEREO (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)
#define UAC_AC_BODY_LEN                                                                                               \
    (TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN +              \
     TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(0))

#define USB_AUDIO_DESC_LEN                                                                                            \
    (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN + UAC_AC_BODY_LEN +          \
     TUD_AUDIO20_DESC_STD_AS_LEN * 2 + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN +          \
     TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

/* bInterval: 1 ms either way (full speed in frames, high speed as 2^(n-1) microframes). */
#define USB_AUDIO_DESC(_itf, _interval)                                                                               \
    TUD_AUDIO20_DESC_IAD(_itf, 2, 0),                                                                                 \
    TUD_AUDIO20_DESC_STD_AC(_itf, 0, USB_AUDIO_STRING),                                                               \
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_HEADSET, UAC_AC_BODY_LEN, 0),                                         \
    TUD_AUDIO20_DESC_CLK_SRC(UAC_CLK, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,                                           \
                             (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), UAC_IT, 0),                   \
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_IT, AUDIO_TERM_TYPE_USB_STREAMING, 0, UAC_CLK, 2, UAC_STEREO, 0, 0, 0),           \
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_OT, AUDIO_TERM_TYPE_OUT_HEADPHONES, UAC_IT, UAC_FU, UAC_CLK, 0, 0),              \
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC_FU, UAC_IT, 0,                                                                  \
                                  AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS |                             \
                                      AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS),                       \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 1), 0, 0, 0),                                                      \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 1), 1, 1, 0),                                                      \
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_IT, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM, 2,   \
                               UAC_STEREO, 0),                                                                        \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(2, 16),                                                                            \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(USB_AUDIO_EP_OUT,                                                                  \
                                   (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA), \
                                   USB_AUDIO_EP_SIZE, _interval),                                                     \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,                \
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0)

extern const usbd_class_driver_t usb_audio_driver;
bool usb_audio_init(void);
