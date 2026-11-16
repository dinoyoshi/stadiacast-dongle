# Stadiacast dongle

Google shut Stadia down, but the controllers are still great. Flip one to its Wi-Fi mode and
it can talk to something other than Google's servers. This project makes that something a
small microcontroller board you plug into USB.

The board runs this firmware. It puts up its own Wi-Fi hotspot, the Stadia Controller connects
to it, and your PC, Switch or phone just sees a wired USB controller. Nothing to install on the
computer, no drivers, no cloud.

```
Stadia Controller  ))) Wi-Fi )))  dongle board  ── USB ──  PC / Switch / phone
                                  (its own hotspot)        (sees a USB gamepad)
```

The firmware is written to run on more than one board: everything the controller and the
computer see lives in a portable core, and each board only adds its own Wi-Fi, USB and storage.

## What it does

- **Every button, stick and trigger**, with the game's **rumble** sent back to the controller.
- **Headset audio**: plug headphones into the controller and pick the dongle as your sound
  output. The headset's **microphone** becomes the computer's microphone, with push to talk on
  the Assistant button.
- **Four controller types**, switched by holding Stadia + A, B, X or Y: Xbox 360 (the default),
  DualShock 4, Switch Pro, or a plain HID gamepad. The dongle remembers your choice.
- **Up to four controllers at once**, each showing up as its own gamepad.
- **A web console** at [dinoyoshi.github.io/stadiacast-dongle](https://dinoyoshi.github.io/stadiacast-dongle/).
  Open it in Chrome or Edge with the dongle plugged in and you get live stats (battery,
  connection quality, audio), settings, a rumble test, **pairing a new controller** over the
  board's Bluetooth, and **firmware updates** that roll back on their own if something goes
  wrong.

## Supported boards

| Board | Status | Wi-Fi and Bluetooth | USB to the computer | Guide |
|---|---|---|---|---|
| DFRobot FireBeetle 2 ESP32-P4 (DFR1172) | Supported | On-board ESP32-C6 over SDIO (ESP-Hosted) | ESP32-P4 high-speed OTG port (TinyUSB) | [firmware/esp32p4](firmware/esp32p4/README.md) |
| Linux PC (host build) | For testing only: runs the core against a fake controller, no USB gamepad | The PC's network | none | [firmware/README.md](firmware/README.md#building-and-testing-the-core-on-a-pc) |
| Your board | Welcome! | A Wi-Fi access point the controller can join | A USB device port | [Adding a board](firmware/README.md#adding-a-board) |

A new board needs a Wi-Fi access point, a USB device port (high speed if you want headset
audio), a little key/value storage, and optionally Bluetooth LE for pairing and a second
firmware slot for updates. Any ESP32 can reuse the ESP-IDF layer as it is.
[firmware/README.md](firmware/README.md) walks through it, with the FireBeetle as the worked
example.

## Features

What each board can do. ✅ works and was tried on real hardware, 🧪 works in the PC test build
only, ⚪ built but not tried yet, — not available.

| Feature | FireBeetle 2 ESP32-P4 | Linux host build |
|---|---|---|
| Controller connects over Wi-Fi | ✅ | 🧪 |
| Buttons, sticks, triggers | ✅ | 🧪 |
| Rumble | ✅ | 🧪 |
| Headset audio (computer to controller) | ✅ | 🧪 |
| Headset microphone (controller to computer) | ✅ | 🧪 |
| Four USB controller modes | ✅ | 🧪 (reports only) |
| Several controllers at once | ✅ with one real controller; players 2 to 4 ⚪ | 🧪 two fake controllers |
| Web console | ✅ | 🧪 (protocol only) |
| Pairing a controller from the console | ✅ | 🧪 fake controller |
| Firmware updates with rollback | ✅ | 🧪 |
| Status LED and button | ✅ | — |

### Controller modes

| Mode | USB ID | Rumble | Headset | One gamepad per player | Works with |
|---|---|---|---|---|---|
| Xbox 360 (default) | 045E:028E, or 1209:0001 with the headset on | yes | yes | yes | Windows, Linux, SteamOS, Android |
| DualShock 4 | 054C:09CC | yes | yes | no, player 1 only | PC games and Steam with PlayStation prompts (not a real PS4) |
| Switch Pro | 057E:2009 | yes | no | no, player 1 only | Nintendo Switch (wired), Steam, Linux |
| HID gamepad | 303A:4047 | no | yes | yes | macOS, Android, older DirectInput games |

### Tested on

What has been tried with a real controller and a real board, by the computer it was plugged into.

| | Linux | Windows | macOS | Nintendo Switch |
|---|---|---|---|---|
| Buttons, sticks, triggers | ✅ all four modes | ✅ | ⚪ | ⚪ |
| Rumble | ✅ Xbox 360 mode (DS4 and Switch Pro ⚪) | ⚪ | — (HID mode has none) | ⚪ |
| Headset audio | ✅ | ✅ Xbox 360, DS4 and HID modes | ⚪ | — |
| Headset microphone | ✅ | ⚪ | ⚪ | — |
| Web console (Chrome or Edge) | ✅ | ⚪ | ⚪ | — |

Known rough edges: some loud songs clip a little in the headset (lowering the gain in the
console a few dB should help), and Steam shows Xbox 360 mode with the headset on as a generic
"Xbox Pad". The full log of what was built, tested and learned is in
[progress.md](firmware/esp32p4/docs/progress.md). If you try something marked ⚪, an issue
saying how it went is very welcome.

## Quick start

You also need a Stadia Controller on its **Wi-Fi firmware** (a controller that was switched to
Bluetooth mode has to be flashed back first, [how](firmware/esp32p4/docs/step1-baseline.md)),
and Chrome or Edge on a desktop for the web console. These steps are for the FireBeetle 2
ESP32-P4; other boards will have their own guide.

1. **Flash the board once.** Download
   [stadiacast-dongle-full.bin](https://dinoyoshi.github.io/stadiacast-dongle/firmware/stadiacast-dongle-full.bin)
   (CI builds it from `main`) and write it at address `0x0` over the board's programming
   USB-C port, either in the browser with [ESP Web Tool](https://espressif.github.io/esptool-js/)
   or with `esptool --chip esp32p4 write_flash 0x0 stadiacast-dongle-full.bin`. You can also
   [build it yourself](firmware/esp32p4/README.md#build-and-flash) with ESP-IDF 5.5.
2. **Plug the other USB-C port (high-speed OTG) into your computer.** That one is the dongle
   from now on. Open the [web console](https://dinoyoshi.github.io/stadiacast-dongle/), press
   **Connect** and pick the dongle's serial port.
3. **Pair your controller.** Turn it off, then hold Y and press Stadia until the light pulses
   orange. In the console's Controllers card press **Pair a controller**. The dongle hands the
   controller its hotspot's name and password, and the controller joins it about 20 seconds
   later.
4. **Play.** From now on, plug in the dongle, wait for the LED to blink slowly, and press the
   Stadia button. The LED stays on while the controller is connected.

Later updates come from the console's **Install latest** button, over the same cable.

The [FireBeetle user guide](firmware/esp32p4/README.md) covers all of this in more detail,
plus the button mapping for each mode, headset setup on Linux and Windows, every setting, and
what the LED means.

## For developers

Clone with submodules (libopus is one):

```sh
git clone --recurse-submodules https://github.com/dinoyoshi/stadiacast-dongle.git
```

The firmware is split into a portable core (the controller's protocol, the gamepad modes, the
dongle's logic and the web console's protocol) and a thin layer per board. [firmware/README.md](firmware/README.md)
explains the layout and walks through adding a board or a gamepad mode.

The core also builds on Linux and runs against a fake controller, the same way CI does:

```sh
uv sync --locked                      # Python environment for the fake controller
cmake -S firmware/host -B build-host -DMBEDTLS_DIR=/path/to/mbedtls-3.6
cmake --build build-host -j
build-host/gamepad_test && build-host/dongle_test && firmware/host/run_loopback.sh build-host
```

## Credits

This is a port of [Stadiacast](https://github.com/jkfran/stadiacast) by jkfran, a Python
receiver for the Stadia Controller's Wi-Fi mode. All the hard protocol work is theirs: this
firmware speaks what Stadiacast worked out, and its
[protocol notes](https://github.com/jkfran/stadiacast/blob/main/docs/protocol.md) are the
reference. When a comment in the firmware mentions `src/stadiacast/...` or `docs/...`, it
means that repository.

The firmware started life in a fork, [dinoyoshi/stadiacast](https://github.com/dinoyoshi/stadiacast),
which still has its first 34 pull requests. It moved here, history and all, on 2026-11-16.

Headset audio uses the [Opus](https://opus-codec.org/) codec from Xiph.Org.

## License

MIT, like Stadiacast. See [LICENSE](LICENSE), and [NOTICE](NOTICE) for the parts that aren't
ours (the protocol field numbers are Google's, libopus is BSD).

This is an independent interoperability project. It is not affiliated with, endorsed by or
supported by Google. Stadia is a trademark of Google LLC.
