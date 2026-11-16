/* Small helpers the gamepad modes share. */
#pragma once

#include "gamepad.h"

#include <stdbool.h>
#include <stdint.h>

/* HID hat: 0 = up, clockwise to 7 = up-left, 8 = centred. */
static inline uint8_t hat(uint16_t b)
{
    bool u = b & USB_PAD_UP, d = b & USB_PAD_DOWN, l = b & USB_PAD_LEFT, r = b & USB_PAD_RIGHT;
    if (u && r) return 1;
    if (d && r) return 3;
    if (d && l) return 5;
    if (u && l) return 7;
    if (u) return 0;
    if (r) return 2;
    if (d) return 4;
    if (l) return 6;
    return 8;
}

/* -32768..32767 (up positive) to 0..255 (down positive when flip) */
static inline uint8_t axis8(int16_t v, bool flip)
{
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(flip ? 255 - x : x);
}

static inline int16_t neg(int16_t v)
{
    return v == -32768 ? 32767 : (int16_t)-v;
}

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF, p[1] = v >> 8;
}
