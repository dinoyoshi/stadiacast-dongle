# Stadiacast dongle

Google shut Stadia down, but the controllers are still great. Flip one to its Wi-Fi mode and
it can talk to something other than Google's servers. This project makes that something a
small board you plug into USB.

The dongle is a **DFRobot FireBeetle 2 ESP32-P4** running this firmware. It puts up its own
Wi-Fi hotspot, the Stadia Controller connects to it, and your PC, Switch or phone just sees a
wired USB controller. Nothing to install on the computer, no drivers, no cloud.

```
Stadia Controller  ))) Wi-Fi )))  FireBeetle 2 ESP32-P4  ── USB-C ──  PC / Switch / phone
                                  (hotspot on its ESP32-C6)          (sees a USB gamepad)
```

## What it does

- **Every button, stick and trigger**, with the game's **rumble** sent back to the controller.
- **Headset audio**: plug headphones into the controller and pick the dongle as your sound
  output. The headset's **microphone** becomes the PC's microphone, with push to talk on the
  Assistant button.
- **Four controller types**, switched by holding Stadia + A, B, X or Y: Xbox 360 (the default),
  DualShock 4, Switch Pro, or a plain HID gamepad. The dongle remembers your choice.
- **Up to four controllers at once**, each showing up as its own gamepad (Xbox 360 and HID modes).
- **A web console** at [dinoyoshi.github.io/stadiacast-dongle](https://dinoyoshi.github.io/stadiacast-dongle/).
  Open it in Chrome or Edge with the dongle plugged in and you get live stats (battery,
  connection quality, audio), settings, a rumble test, **pairing a new controller** over the
  board's own Bluetooth, and **firmware updates** that roll back on their own if something
  goes wrong.
- Runs from a single USB-C cable, with an LED that tells you what it's doing.

## What you need

- A DFRobot FireBeetle 2 ESP32-P4 (DFR1172). It
  has the ESP32-C6 for Wi-Fi and Bluetooth on board, plus the LED and BOOT button the firmware
  uses. Nothing to solder.
- A USB-C cable for its **high-speed OTG port** (that's the dongle's port), and for the first
  flash a cable on the other, programming port.
- A Stadia Controller on its **Wi-Fi firmware**. A controller that was switched to Bluetooth
  mode has to be flashed back first
  ([how](firmware/esp32p4/docs/step1-baseline.md)).
- Chrome or Edge on a desktop for the web console.

## Quick start

1. **Flash the board once.** Download
   [stadiacast-dongle-full.bin](https://dinoyoshi.github.io/stadiacast-dongle/firmware/stadiacast-dongle-full.bin)
   (CI builds it from `main`) and write it at address `0x0` over the board's programming port,
   either in the browser with [ESP Web Tool](https://espressif.github.io/esptool-js/) or with
   `esptool --chip esp32p4 write_flash 0x0 stadiacast-dongle-full.bin`. You can also
   [build it yourself](firmware/esp32p4/README.md#build-and-flash) with ESP-IDF 5.5.
2. **Plug the OTG port into your computer** and open the
   [web console](https://dinoyoshi.github.io/stadiacast-dongle/). Press **Connect** and pick
   the dongle's serial port.
3. **Pair your controller.** Turn it off, then hold Y and press Stadia until the light pulses
   orange. In the console's Controllers card press **Pair a controller**. The dongle hands the
   controller its hotspot's name and password, and the controller joins it about 20 seconds
   later.
4. **Play.** From now on, plug in the dongle, wait for the LED to blink slowly, and press the
   Stadia button. The LED stays on while the controller is connected.

Later updates come from the console's **Install latest** button, over the same cable.

The [user guide](firmware/esp32p4/README.md) covers all of this in more detail, plus the
button mapping for each mode, headset setup on Linux and Windows, every setting, and what the
LED means.

## How well it works

Everything below was tried on a real board unless it says otherwise.

| | |
|---|---|
| Buttons, sticks, triggers | Works on Linux and Windows |
| Rumble | Works in Xbox 360 mode; DualShock 4 and Switch Pro rumble not tested yet |
| Headset audio | Works on Linux and Windows. Some loud songs clip a little; lowering the gain in the console a few dB should help |
| Headset microphone | Works on Linux. **Not tested on Windows yet** |
| Controller modes | All four work on Linux, and Steam sees DualShock 4 mode as a PS4 controller. Switch Pro mode has not been tried on a real Switch |
| Pairing from the console | Works |
| Firmware updates from the console | Works, including rollback |
| Several controllers | Works with fake controllers on a PC, and with one real controller on the board. **Players 2 to 4 with real controllers are not tested yet** (I only have one) |

The full log of what was built, tested and learned along the way is in
[progress.md](firmware/esp32p4/docs/progress.md). If you try something from the untested
list, an issue saying how it went is very welcome.

## For developers

Clone with submodules (libopus is one):

```sh
git clone --recurse-submodules https://github.com/dinoyoshi/stadiacast-dongle.git
```

The firmware is split into a portable core (the controller's protocol, the gamepad modes, the
dongle's logic and the web console's protocol) and a thin board layer, so a different board
only has to bring its own Wi-Fi, USB and storage. [firmware/README.md](firmware/README.md)
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
