---
name: port-board
description: Port the Stadiacast dongle firmware to a new microcontroller board. Use when asked to add support for a board, chip or USB/Wi-Fi stack other than the FireBeetle 2 ESP32-P4.
---

# Port the dongle to a new board

Read `AGENTS.md` (layout and hard-won rules) and the "Adding a board" section of
`firmware/README.md` first. This is the step-by-step version. The FireBeetle project
(`firmware/esp32p4/`) is the worked example for every step: open the matching file there
before writing each piece.

## 0. Check the board can do it

Ask for, or look up, the board's datasheet and answer these before writing code. Stop and
report if a must-have is missing.

| Need | Must have? | Notes |
|---|---|---|
| A Wi-Fi access point the controller can join | yes | The board gets a fixed address on it |
| A USB device port | yes | High speed (480 Mbit/s) for headset audio; full speed is enough for input and rumble only |
| Enough RAM for TLS, DTLS-SRTP and Opus | yes | The P4 has 768 KB internal RAM plus 32 MB PSRAM for bulk buffers; Opus state needs fast (internal) RAM. Check the FireBeetle's heap figures in the console's stats before guessing |
| CPU for Opus encode at 48 kHz stereo, complexity <= 8, in under ~10 ms per 20 ms frame | for audio | Without it, ship input and rumble only |
| A key/value store | yes | Settings, players, the USB mode |
| Bluetooth LE central | no | Pairing from the console; without it, `stadiacast-provision` on a PC still pairs |
| Room for two firmware slots | no | Console updates; without it, `board_fw_begin()` returns an error |
| An LED and a button | no | |

Write down which optional features the port will have. Name the new board folder
`firmware/<board>` (for example `firmware/esp32s3-devkit`).

## 1. The OS layer

`firmware/platform/port_api/include/stadia_port.h`: tasks, time, memory (`port_alloc_fast()` =
internal RAM, `port_alloc_bulk()` = PSRAM or whatever is slower), locks, events, timers, byte
streams, inflate/gunzip, log.

- Any ESP32 on ESP-IDF: use `firmware/platform/port_esp_idf` as it is.
- Another OS (Zephyr, Pico SDK + FreeRTOS, ...): add `firmware/platform/port_<os>/`
  implementing every function in the header. Use `port_posix` as a second reference.

## 2. Board services

Implement `firmware/core/dongle/include/dongle_board.h`. On ESP-IDF this must be a component
named `dongle_board`, because the core links against that name; copy the shape of
`firmware/esp32p4/components/dongle_board/`.

- Key/value store (NVS on ESP32).
- LED and button, or stubs.
- USB mode and reports (calls into your USB stack from step 4).
- Console link: a USB CDC serial port next to the gamepad, or none.
- Board info and stats, restart. `board_tasks_json()` may disable interrupts; the core
  already only calls it while the headphones are idle. Never add a periodic caller.
- Firmware slots, or `board_fw_begin()` returning an error.

`firmware/host/dongle_test.c` has a fake board with every call: read it to see what each one is
expected to do.

## 3. Wi-Fi access point

The controller joins it; the board needs a fixed address and DNS answers pointing at itself
(the core's `dns_sink.c` does the DNS). Model: `firmware/esp32p4/main/hotspot.c`. Keep the
DTIM period at 1, or audio stutters.

## 4. USB device

A stack that sends the reports `core/gamepad` builds and takes the host's output reports
(rumble). For headset audio, a UAC2 function: speaker (iso OUT) and microphone (iso IN).
Model: `firmware/esp32p4/components/usb_pad` (TinyUSB). Check the AGENTS.md rules on USB IDs
and iso OUT bInterval.

## 5. Bluetooth LE (optional)

A `dongle_ble_t` (`core/dongle/include/dongle_core.h`): scan with every advertisement reported,
connect, find characteristics by 128-bit UUID, write (long when needed), long read,
notifications, link loss reported back. Model: `firmware/esp32p4/main/ble_central.c` (NimBLE).
The protocol itself is in `core/dongle/pair.c`; don't copy it into the board.

## 6. Start-up

Model: `firmware/esp32p4/main/main.c`.

1. `settings_load()`, then `stadia_start()` with the hotspot's settings.
2. Wire input (`dongle_pad_state()`, `pad_mode_filter()`), rumble (`rumble_host()`) and audio
   between the core and the USB side.
3. With Bluetooth: `pair_setup()` and `console_set_pairing()`. Then `console_start()`.
4. With firmware slots: `ota_confirm_when_up()`.

Large buffers on the heap, not static. Opus and audio task stacks in fast RAM.

## 7. CI

Add a job that builds the new board on every pull request, next to the FireBeetle's in
`.github/workflows/console-pages.yml` (or a new workflow with the same `paths` filters:
`firmware/core/**`, `firmware/platform/**` and the board folder). Pin the toolchain version.
Only the FireBeetle's image is published to Pages unless asked otherwise.

## 8. Docs

- `firmware/<board>/README.md`: what to buy, how to build and flash, which features work.
- Root `README.md`: a row in "Supported boards" and a column in "Features" (mark untested
  features as built but not tried).
- `firmware/README.md`: the board in the layout tree.
- A row in the Timeline of `firmware/esp32p4/docs/progress.md`.

## Done when

- [ ] No file under `firmware/core/` includes anything board- or SDK-specific (grep for
      `esp_`, `freertos`, the board's SDK headers).
- [ ] The host tests pass: `gamepad_test -v`, `dongle_test`, `run_loopback.sh` (commands in
      AGENTS.md). The core change for the port, if any, didn't change a mode's report hash.
- [ ] The FireBeetle still builds (its CI job is green).
- [ ] The new board builds in CI.
- [ ] The PR says which features were tried on the real board and which were not.
