# Stadiacast dongle

Firmware that turns a **DFRobot FireBeetle 2 ESP32-P4** board into a USB dongle for the Stadia
Controller, so the controller needs no PC software at all. The board hosts its own Wi-Fi hotspot
on its ESP32-C6, the controller connects to it, and the board plugs into a PC, console or phone
as a wired USB controller:

- every button, stick and trigger, and the game's rumble back to the controller;
- a USB headset: the PC's sound plays on the controller's headset jack, and the headset's
  microphone is the PC's microphone (push to talk: hold the Assistant button);
- four modes, switched with Stadia + A/B/X/Y: Xbox 360 (XInput), DualShock 4, Switch Pro and a
  plain HID gamepad, remembered across power-ups;
- runs from one USB-C port, with an LED for its state;
- up to four controllers at once, one USB gamepad each (Xbox 360 and HID modes);
- a [web console](https://dinoyoshi.github.io/stadiacast-dongle/) in Chrome or Edge, over the
  same USB cable: live stats (battery, connection quality, audio), settings (mode, headphone gain
  and bitrate, rumble strength, microphone), pairing a controller over the board's own Bluetooth,
  and firmware updates with automatic rollback.

It is tested on the board with Linux and Windows.

- **User guide** (build, flash, first-time setup, button mapping, web console):
  [firmware/esp32p4/README.md](firmware/esp32p4/README.md)
- **Where it stands**: [firmware/esp32p4/docs/progress.md](firmware/esp32p4/docs/progress.md)
- **Code layout and porting to another board**: [firmware/README.md](firmware/README.md)

Clone with `git clone --recurse-submodules` (libopus is a submodule), or run
`git submodule update --init` in an existing checkout.

## Tests

The portable core builds for Linux and runs against a fake controller, as CI does
(`.github/workflows/firmware-host.yml`):

```sh
uv sync --locked                      # the fake controller's Python environment
cmake -S firmware/host -B build-host -DMBEDTLS_DIR=/path/to/mbedtls-3.6
cmake --build build-host -j
build-host/gamepad_test && build-host/dongle_test && firmware/host/run_loopback.sh build-host
```

## Credits

This is a port of [Stadiacast](https://github.com/jkfran/stadiacast), jkfran's Python receiver
for the Stadia Controller's Wi-Fi mode. The protocol work is Stadiacast's, and its
[docs/protocol.md](https://github.com/jkfran/stadiacast/blob/main/docs/protocol.md) is the
reference for what the firmware speaks. Paths such as `src/stadiacast/...` or `docs/...` in the
firmware's comments refer to that repository.

The firmware was developed in a fork of Stadiacast, [dinoyoshi/stadiacast](https://github.com/dinoyoshi/stadiacast),
where its pull requests #1 to #34 are, and moved here with its history on 2026-11-16.

MIT License, see [LICENSE](LICENSE) and [NOTICE](NOTICE). Stadiacast is an independent
interoperability project, not affiliated with Google. Stadia is a trademark of Google LLC.
