/*
 * Picking the gamepad mode from the controller: hold the Stadia button with a face button for
 * 1.5 s (a 4 s hold of the Stadia button turns the controller off). The board leaves USB and
 * comes back as the new kind of controller, remembers it (NVS) for the next power-up, and
 * pulses the controller's motors once per mode number:
 *   Stadia + A   Xbox 360      (1 pulse)
 *   Stadia + B   DualShock 4   (2 pulses)
 *   Stadia + X   Switch Pro    (3 pulses)
 *   Stadia + Y   HID gamepad   (4 pulses)
 * While the Stadia button is held with a face button, neither goes to the host.
 * A press of the board's button (BOOT, GPIO35, on the FireBeetle 2 ESP32-P4) steps to the next
 * mode, controller or not. The board's LED blinks the mode number either way.
 */
#include "dongle_board.h"
#include "dongle_core.h"
#include "pad_modes.h"
#include "stadia_port.h"

#include <stdint.h>

static const char *TAG = "pad_mode";

#define HOLD_US (1500 * 1000)
#define TICK_US (100 * 1000)
#define FACE (USB_PAD_A | USB_PAD_B | USB_PAD_X | USB_PAD_Y)

/* The controller reports only changes, so a steady hold is timed by a 100 ms tick. */
static struct {
    volatile usb_pad_mode_t want;
    volatile int64_t since;   /* 0: no combo held */
    volatile bool latched;    /* fired: wait for the buttons to be let go */
    volatile bool busy;
    port_timer_t *tick;
    int boot_low; /* ticks the button has been down */
} M;

usb_pad_mode_t pad_mode_load(usb_pad_mode_t default_mode)
{
    usb_pad_mode_t mode = default_mode;
    uint8_t v;
    if (board_kv_get_u8("pad", "mode", &v) && v < USB_PAD_MODE_COUNT) mode = v;
    return mode;
}

static void switch_task(void *arg)
{
    usb_pad_mode_t mode = (usb_pad_mode_t)(intptr_t)arg;
    SLOGI(TAG, "switching to %s", pad_mode_info(mode)->name);
    if (board_usb_set_mode(mode)) board_kv_set_u8("pad", "mode", (uint8_t)mode);
    status_led_flash((int)board_usb_mode() + 1);
    for (int i = 0; i <= (int)board_usb_mode(); i++) {
        stadia_rumble(0, 40000, 40000);
        port_sleep_ms(200);
        stadia_rumble(0, 0, 0);
        port_sleep_ms(250);
    }
    M.busy = false;
}

static void start_switch(usb_pad_mode_t mode)
{
    M.busy = true;
    if (!port_spawn(switch_task, (void *)(intptr_t)mode, "pad_mode", 4096)) M.busy = false;
}

bool pad_mode_request(usb_pad_mode_t mode)
{
    if (mode >= USB_PAD_MODE_COUNT || M.busy) return false;
    if (mode == board_usb_mode()) return true;
    start_switch(mode);
    return M.busy;
}

static void tick(void *arg)
{
    /* the board's button: a press (two ticks down) steps to the next mode */
    if (board_button_down()) {
        if (++M.boot_low == 2 && !M.busy) {
            start_switch((usb_pad_mode_t)((board_usb_mode() + 1) % USB_PAD_MODE_COUNT));
        }
    } else {
        M.boot_low = 0;
    }
    int64_t since = M.since;
    if (!since || M.latched || M.busy || port_now_us() - since < HOLD_US) return;
    M.latched = true;
    start_switch(M.want);
}

void pad_mode_start(void)
{
    board_button_init();
    if (!(M.tick = port_timer_new(tick, NULL, "pad_mode")) || !port_timer_start_periodic(M.tick, TICK_US)) {
        SLOGE(TAG, "no timer: the mode combos and the board's button won't work");
    }
}

void pad_mode_filter(usb_pad_state_t *st)
{
    uint16_t face = st->buttons & FACE;
    if (!(st->buttons & USB_PAD_GUIDE) || !face) {
        M.since = 0;
        M.latched = false;
        return;
    }
    st->buttons &= (uint16_t)~(USB_PAD_GUIDE | FACE); /* the combo is ours */
    usb_pad_mode_t want = face == USB_PAD_A   ? USB_PAD_XINPUT
                          : face == USB_PAD_B ? USB_PAD_DS4
                          : face == USB_PAD_X ? USB_PAD_SWITCH
                          : face == USB_PAD_Y ? USB_PAD_HID
                                              : USB_PAD_MODE_COUNT;
    if (want == USB_PAD_MODE_COUNT) { /* two face buttons: not a combo */
        M.since = 0;
        return;
    }
    if (!M.since || want != M.want) {
        M.want = want;
        M.since = port_now_us();
    }
}
