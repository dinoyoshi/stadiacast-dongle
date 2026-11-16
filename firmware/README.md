# Stadiacast firmware

Firmware that turns a microcontroller board into a USB dongle for the Stadia Controller: the
controller joins the board's Wi-Fi hotspot, and the board is a USB gamepad (with rumble, a
headset and a microphone) to the computer. The first and so far only board is the DFRobot
FireBeetle 2 ESP32-P4; its user guide is [esp32p4/README.md](esp32p4/README.md).

The code is split so that another board only has to bring what is tied to its hardware:

```
firmware/
  core/                portable C: no ESP-IDF, no board
    stadia/            the controller's side: HTTPS, signalling, ICE/DTLS-SRTP, the display
                       and game sessions, Opus headset audio, the microphone, several players
    gamepad/           the USB gamepad modes (descriptors, reports, the DS4's and Switch Pro's
                       USB protocols), one mode_<name>.c each
    dongle/            the dongle's own logic: input mapping, the mode combos and the board's
                       button, rumble settings and test, players, status LED patterns; the
                       board provides dongle_board.h (key/value store, LED, button, USB)
    opus/              libopus 1.5.2 (git submodule), fixed point
  platform/
    port_api/          stadia_port.h: the OS services the core needs
    port_esp_idf/      stadia_port.h on ESP-IDF (any ESP32)
    port_posix/        stadia_port.h on Linux (the host build)
  host/                the core built for Linux and tested against tests/fake_controller.py
  esp32p4/             the FireBeetle 2 ESP32-P4: an ESP-IDF project with the board's own
                       code (C6 hotspot, TinyUSB on the P4's high-speed port, dongle_board.h
                       on NVS and its GPIOs, console, OTA, NimBLE pairing) and the web
                       console page (console/)
```

The core's sources build unchanged for every target: as ESP-IDF components on an ESP32 board
(`esp32p4/CMakeLists.txt` adds them with `EXTRA_COMPONENT_DIRS`), and as static libraries
elsewhere (`host/CMakeLists.txt`).

## Building and testing the core on a PC

From the repository root (needs CMake, a C compiler, zlib, an mbedTLS 3.6 source tree, and `uv
sync` for the fake controller):

```sh
git submodule update --init
cmake -S firmware/host -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls
cmake --build build-host
build-host/gamepad_test                  # every gamepad mode's reports, against known hashes
build-host/dongle_test                   # core/dongle on a fake board
firmware/host/run_loopback.sh build-host # a whole controller session against the fake controller
```

The `Firmware core on Linux` workflow runs all three on every pull request that touches `firmware/`.

## Adding a gamepad mode

1. A value in `usb_pad_mode_t` (`core/gamepad/include/gamepad.h`), before `USB_PAD_MODE_COUNT`.
2. `core/gamepad/mode_<name>.c` with its `pad_mode_t`: USB IDs, the HID report descriptor, the
   input report, and what it does with the host's output reports (rumble).
3. Its entry in the table in `core/gamepad/pad_modes.c`, and the file in
   `core/gamepad/CMakeLists.txt`.
4. Run `build-host/gamepad_test -v` and add the new mode's hash to `host/gamepad_test.c`.
5. A Stadia + button combo in `core/dongle/pad_mode.c` if it should have one, and on the
   ESP32-P4 the mode's number in `esp32p4/main/Kconfig.projbuild` (`DONGLE_PAD_MODE`) and its
   name in the web console's mode list (`esp32p4/console/index.html`).

## Adding a board

Still in progress: the console and OTA protocols and pairing still live in `esp32p4/main/` and
move into `core/` next. For now a new board needs:

- an implementation of `platform/port_api/include/stadia_port.h` (or `port_esp_idf` on any
  ESP32, `port_posix` on Linux),
- an implementation of `core/dongle/include/dongle_board.h`: a key/value store, the LED and
  button (or none), and its USB device's mode and reports. On ESP-IDF it is a component named
  `dongle_board` (the ESP32-P4's is `esp32p4/components/dongle_board`),
- a Wi-Fi access point the controller joins, with the board at a fixed address,
- a USB device stack that sends the reports `core/gamepad` builds (on TinyUSB, the ESP32-P4's
  `esp32p4/components/usb_pad` is the model),
- a start-up that calls `stadia_start()` and passes input (`dongle_pad_state()`,
  `pad_mode_filter()`), rumble (`rumble_host()`) and audio between the core and the USB side,
  as `esp32p4/main/main.c` does.
