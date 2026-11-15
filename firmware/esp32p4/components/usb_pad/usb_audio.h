/*
 * The USB Audio Class 2.0 headset next to the gamepad interface (usb_audio.c): headphones, one
 * 48 kHz 16-bit stereo OUT stream (adaptive, no feedback endpoint), and a microphone, one 48 kHz
 * 16-bit mono IN stream (asynchronous), each with the host's volume and mute.
 */
#pragma once

#include "tinyusb.h"
#include "device/usbd_pvt.h"
#include "class/audio/audio.h"

#define USB_AUDIO_EP_OUT 0x02
#define USB_AUDIO_EP_IN 0x82
/* One packet per (micro)frame: 48 + 1 frames of 4 bytes per 1 ms at full speed, 6 + 1 per 125 us
 * microframe at high speed. bInterval is 1 at both speeds: the DWC2 core arms an iso OUT
 * transfer for the next microframe's parity, so a packet every 8 microframes (bInterval 4)
 * lands on the other parity half the time and never completes. */
#define USB_AUDIO_EP_SIZE_FS 196
#define USB_AUDIO_EP_SIZE_HS 28
#define USB_AUDIO_EP_SIZE USB_AUDIO_EP_SIZE_FS /* the buffer: the larger of the two */
/* The microphone, mono: 48 (+1 room) samples of 2 bytes per 1 ms, 6 (+1) per microframe. */
#define USB_MIC_EP_SIZE_FS 98
#define USB_MIC_EP_SIZE_HS 14
#define USB_MIC_EP_SIZE USB_MIC_EP_SIZE_FS
#define USB_AUDIO_STRING 4    /* "Controller headset" */

/* entities. Each feature unit carries bmaControls for the master channel and every logical
 * channel of its cluster (2 for the headphones, 1 for the microphone): Windows' usbaudio2 checks
 * the descriptor length against the channel count and does not start the device when they
 * disagree; Linux accepts a master-only unit. Only the master channel has controls. */
#define UAC_IT 0x01
#define UAC_FU 0x02
#define UAC_OT 0x03
#define UAC_CLK 0x04
#define UAC_MIC_IT 0x05
#define UAC_MIC_FU 0x06
#define UAC_MIC_OT 0x07

#define UAC_STEREO (AUDIO20_CHANNEL_CONFIG_FRONT_LEFT | AUDIO20_CHANNEL_CONFIG_FRONT_RIGHT)
#define UAC_MONO AUDIO20_CHANNEL_CONFIG_FRONT_CENTER
#define UAC_AC_BODY_LEN                                                                                               \
    (TUD_AUDIO20_DESC_CLK_SRC_LEN + TUD_AUDIO20_DESC_INPUT_TERM_LEN * 2 + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN * 2 +      \
     TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(2) + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(1))

/* one AS interface: alt 0 (no endpoint) and alt 1 with the stream */
#define UAC_AS_LEN                                                                                                    \
    (TUD_AUDIO20_DESC_STD_AS_LEN * 2 + TUD_AUDIO20_DESC_CS_AS_INT_LEN + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN +          \
     TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)

#define USB_AUDIO_DESC_LEN                                                                                            \
    (TUD_AUDIO20_DESC_IAD_LEN + TUD_AUDIO20_DESC_STD_AC_LEN + TUD_AUDIO20_DESC_CS_AC_LEN + UAC_AC_BODY_LEN +          \
     UAC_AS_LEN * 2)
#define USB_AUDIO_ITF_COUNT 3 /* control, headphones, microphone */

#define UAC_FU_CTRLS (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS | AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)

#define USB_AUDIO_DESC(_itf, _epsize, _micsize)                                                                       \
    TUD_AUDIO20_DESC_IAD(_itf, USB_AUDIO_ITF_COUNT, 0),                                                               \
    TUD_AUDIO20_DESC_STD_AC(_itf, 0, USB_AUDIO_STRING),                                                               \
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_HEADSET, UAC_AC_BODY_LEN, 0),                                         \
    TUD_AUDIO20_DESC_CLK_SRC(UAC_CLK, AUDIO20_CLOCK_SOURCE_ATT_INT_FIX_CLK,                                           \
                             (AUDIO20_CTRL_R << AUDIO20_CLOCK_SOURCE_CTRL_CLK_FRQ_POS), 0, 0),                       \
    /* headphones: USB streaming -> feature unit -> headphones */                                                     \
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_IT, AUDIO_TERM_TYPE_USB_STREAMING, 0, UAC_CLK, 2, UAC_STEREO, 0, 0, 0),           \
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_OT, AUDIO_TERM_TYPE_OUT_HEADPHONES, 0, UAC_FU, UAC_CLK, 0, 0),                   \
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC_FU, UAC_IT, 0, UAC_FU_CTRLS, 0, 0),                                             \
    /* microphone: microphone -> feature unit -> USB streaming */                                                     \
    TUD_AUDIO20_DESC_INPUT_TERM(UAC_MIC_IT, AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0, UAC_CLK, 1, UAC_MONO, 0, 0, 0),        \
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC_MIC_OT, AUDIO_TERM_TYPE_USB_STREAMING, 0, UAC_MIC_FU, UAC_CLK, 0, 0),            \
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC_MIC_FU, UAC_MIC_IT, 0, UAC_FU_CTRLS, 0),                                        \
    /* the headphones' stream */                                                                                      \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 1), 0, 0, 0),                                                      \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 1), 1, 1, 0),                                                      \
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_IT, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM, 2,   \
                               UAC_STEREO, 0),                                                                        \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(2, 16),                                                                            \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(USB_AUDIO_EP_OUT,                                                                  \
                                   (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE | TUSB_ISO_EP_ATT_DATA), \
                                   _epsize, 1),                                                                       \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,                \
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0),                            \
    /* the microphone's stream */                                                                                     \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 2), 0, 0, 0),                                                      \
    TUD_AUDIO20_DESC_STD_AS_INT((uint8_t)((_itf) + 2), 1, 1, 0),                                                      \
    TUD_AUDIO20_DESC_CS_AS_INT(UAC_MIC_OT, AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I, AUDIO20_DATA_FORMAT_TYPE_I_PCM,  \
                               1, UAC_MONO, 0),                                                                       \
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(2, 16),                                                                            \
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(USB_AUDIO_EP_IN,                                                                   \
                                   (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), \
                                   _micsize, 1),                                                                      \
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,                \
                                  AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0)

extern const usbd_class_driver_t usb_audio_driver;
bool usb_audio_init(void);
