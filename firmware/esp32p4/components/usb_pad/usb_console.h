/*
 * The console's USB serial port (usb_console.c): a CDC ACM function after the gamepad and the
 * headset. The API the rest of the firmware uses is in usb_pad.h.
 */
#pragma once

#include "tinyusb.h"
#include "device/usbd_pvt.h"

#define USB_CONSOLE_EP_NOTIF 0x83
#define USB_CONSOLE_EP_DATA_OUT 0x04
#define USB_CONSOLE_EP_DATA_IN 0x84
#define USB_CONSOLE_EP_SIZE_FS 64
#define USB_CONSOLE_EP_SIZE_HS 512 /* bulk at high speed: 512, nothing else */
#define USB_CONSOLE_STRING 5       /* "Stadiacast console" */
#define USB_CONSOLE_ITF_COUNT 2

#define USB_CONSOLE_DESC_LEN (8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7)

/* IAD; communication interface 02/02/00 with header, call management, ACM (line coding and
 * control line state) and union descriptors and its notification endpoint; data interface 0A
 * with bulk OUT and IN. _notif_iv: 16 ms (16 frames at full speed, 2^(8-1) microframes at high). */
#define USB_CONSOLE_DESC(_itf, _epsize, _notif_iv)                                                                    \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, (_itf), 2, TUSB_CLASS_CDC, 0x02, 0x00, 0,                                    \
    9, TUSB_DESC_INTERFACE, (_itf), 0, 1, TUSB_CLASS_CDC, 0x02, 0x00, USB_CONSOLE_STRING,                            \
    5, TUSB_DESC_CS_INTERFACE, 0x00, 0x20, 0x01,                 /* header, CDC 1.20 */                              \
    5, TUSB_DESC_CS_INTERFACE, 0x01, 0x00, (uint8_t)((_itf) + 1), /* call management: none */                        \
    4, TUSB_DESC_CS_INTERFACE, 0x02, 0x02,                       /* ACM: line coding and line state */               \
    5, TUSB_DESC_CS_INTERFACE, 0x06, (_itf), (uint8_t)((_itf) + 1), /* union */                                      \
    7, TUSB_DESC_ENDPOINT, USB_CONSOLE_EP_NOTIF, TUSB_XFER_INTERRUPT, 8, 0, (_notif_iv),                             \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itf) + 1), 0, 2, TUSB_CLASS_CDC_DATA, 0, 0, 0,                               \
    7, TUSB_DESC_ENDPOINT, USB_CONSOLE_EP_DATA_OUT, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,                       \
    7, TUSB_DESC_ENDPOINT, USB_CONSOLE_EP_DATA_IN, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

extern const usbd_class_driver_t usb_console_driver;
bool usb_console_init(void);
