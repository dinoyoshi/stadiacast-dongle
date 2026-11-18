# Troubleshooting (FireBeetle 2 ESP32-P4)

The common problems and what fixes them. Each answer links to the part of the
[user guide](../README.md) with the details. If none of this helps, please
[open a bug report](https://github.com/dinoyoshi/stadiacast-dongle/issues/new?template=bug_report.yml)
with the firmware version and the console's log.

**First, which LED pattern do you see?** The board's status LED (GPIO3) tells you how far the
dongle got:

| LED | meaning | next |
|---|---|---|
| off | no power, or no firmware | [Nothing happens when I plug it in](#nothing-happens-when-i-plug-it-in) |
| fast blinking (5 per second) | starting | wait about 5 s; if it never stops, [the LED never goes slow](#the-led-never-goes-to-slow-blinking) |
| slow blinking (once a second) | ready, waiting for a controller | wake the controller; if it doesn't connect, [the controller doesn't join](#the-controller-doesnt-connect) |
| steady on | a controller is connected | [no input](#the-computer-sees-no-controller) or [no sound](#no-sound-in-the-headset) |
| a few quick blinks, then back | the mode number after a mode change | [Controller modes](../README.md#controller-modes) |

## Which USB-C port?

The board has two USB-C ports, and they do different jobs:

- The **high-speed OTG port** is the dongle: power, gamepad, headset audio and the web
  console's serial port. Plug this one into the computer to play.
- The **serial / programming port** is only for the first flash (`esptool`, ESP Web Tool,
  `idf.py flash`) and the developer's serial log. Plugged in there, the board runs but the
  computer sees a USB serial chip, not a gamepad.

<!-- PHOTO: board top view with both USB-C ports labelled "OTG (dongle)" and "programming".
     Save as firmware/esp32p4/docs/img/ports.jpg and replace this comment with:
     ![The FireBeetle 2 ESP32-P4's two USB-C ports](img/ports.jpg) -->

If you flashed over the programming port and nothing else seems to happen, move the cable to
the OTG port. You never need both at once.

## Nothing happens when I plug it in

- Try another cable: some USB-C cables carry power only.
- A board straight from the factory runs DFRobot's demo, not the dongle. Flash it once
  ([Quick start](../../../README.md#quick-start), step 1).
- After the flash, unplug the programming port and plug the OTG port in.

## The LED never goes to slow blinking

Fast blinking means the board is still starting: the USB device, then the C6 (the Wi-Fi chip),
then the hotspot and the controller's servers. It normally takes about 5 s.

- **A build of your own with a C6 image in `c6_fw/`** updates the C6's Wi-Fi firmware on its first
  start, then restarts. Leave it plugged in until the LED blinks slowly
  ([Updating the C6](../README.md#updating-the-c6-wi-fi-firmware)).
- **Blinking stops and starts again every few seconds**: the board is restarting. Plug in the
  programming port too and read the log (`idf.py -p <port> monitor`, or any serial terminal at
  115200 baud). SDIO, "slave" or `esp_hosted` errors, or a version mismatch warning, point to
  the C6: see [If the Wi-Fi doesn't come up](../README.md#if-the-wi-fi-doesnt-come-up) and
  [Updating the C6](../README.md#updating-the-c6-wi-fi-firmware).
- **A build of your own that boot-loops before `app_main`**: on v1.x P4 chips the RAM free at
  start-up is small, so a large static buffer is enough to break it. Try the published build
  first. A board with a **v3** P4 needs its own build ([Hardware](../README.md#hardware)).

## The controller doesn't connect

The LED blinks slowly, you wake the controller, and nothing changes.

1. **Is the controller set up for this dongle?** A controller remembers one Wi-Fi network. Pair
   it once from the web console's Controllers card
   ([First-time controller setup](../README.md#first-time-controller-setup)). Pairing again
   replaces the network it knew.
2. **Is it on the Wi-Fi firmware?** A controller that was switched to Bluetooth mode shows up on
   phones and PCs as "StadiaXXXX-XXXX" and can't join any hotspot. It has to be flashed back
   first ([how](step1-baseline.md)).
3. **The first start after pairing** sometimes skips the session. Wait a minute, turn the
   controller off (hold Stadia about 4 s until the light goes out) and wake it again with a
   short press.
4. **Pairing can't find the controller.** It has to be in setup mode right then: off, then hold
   Y and press Stadia, and the light pulses orange. Keep it near the board. Phones and earbuds
   nearby advertise the same Bluetooth service; the dongle skips them, but the Controllers card
   shows each device it tried.
5. **Changed the hotspot's name or password** in your own build? Controllers set up for the old
   one must be paired again.
6. **More controllers than players.** By default the dongle takes one controller. Set
   **Players at once** in the Controllers card and restart the dongle
   ([Several controllers](../README.md#several-controllers)).

The console's log (or the serial log) shows how far the controller got: joining the hotspot
(`station joined`), HTTPS, signalling, DTLS and READY.

## The computer sees no controller

- Use the OTG port ([above](#which-usb-c-port)).
- The LED has to be steady: the gamepad sends neutral input until a controller is connected.
- **Pick the mode for your system.** Xbox 360 (Stadia + A) for Windows, Linux, SteamOS and
  Android; DualShock 4 (Stadia + B) for PlayStation prompts on a PC; Switch Pro (Stadia + X)
  for a Switch; HID (Stadia + Y) for macOS. Hold the combo **1.5 s**, not 4 s (that turns the
  controller off). The BOOT button also steps through the modes.
- **Nintendo Switch:** turn on *System Settings → Controllers and Sensors → Pro Controller Wired
  Communication*, and use Switch Pro mode.
- **Steam** shows Xbox 360 mode with the headset on as a generic "Xbox Pad". That is expected:
  the dongle uses another USB ID then so that Windows' Xbox driver doesn't take the headset too.
- **A real PS4** doesn't accept DualShock 4 mode (it checks for Sony hardware).

## No sound in the headset

- Headphones go in the **controller's** 3.5 mm jack, and the dongle has to be the computer's
  sound **output**.
- **Switch Pro mode has no headset.** Use Xbox 360, DualShock 4 or HID mode.
- **Windows:** in *Settings → System → Sound*, pick the dongle as the output (its name mentions
  the controller headset or the Stadiacast dongle). In Xbox 360 mode the dongle shows up as
  1209:0001 ("Xbox Pad" in Steam) so Windows' Xbox driver doesn't swallow the headset; if
  Windows only shows a game controller and no sound device, update the firmware: builds older
  than the Windows audio fix used the Xbox ID and lost the headset there. Unplug and replug
  after a mode change so Windows picks the new device.
- **Linux:** the output is named after the gamepad, for example "Xbox 360 Controller Analog
  Stereo". Set its volume to **100%**: the dongle applies the volume itself.
- **Quiet or clipping:** the console's Settings card has the headphone gain (-12 to 0 dB). Some
  loud songs clip a little at 0 dB; a few dB lower helps.
- The web console's audio card shows whether frames arrive from the computer and reach the
  controller (headphone frames, underruns).

## The microphone doesn't work

- **Push to talk** is on by default: hold the **Assistant** button while you talk. Turn it off
  in the Settings card to keep the mic open.
- The microphone switch takes effect after **Restart dongle**.
- Pick the dongle as the computer's **input** device. It is tested on Linux; on Windows it is
  untested so far, so a report either way is welcome.

## No serial port on Linux (web console or esptool)

The browser's Connect dialog is empty, or `esptool` says permission denied.

- Add yourself to the group that owns serial ports, then **log out and back in**:

  ```sh
  sudo usermod -aG dialout $USER     # Debian, Ubuntu, Fedora
  sudo usermod -aG uucp $USER        # Arch
  ```

  `ls -l /dev/ttyACM*` shows the group. On fish, `$USER` works the same.
- The dongle's console port is a `/dev/ttyACM*` named after the gamepad. The programming port
  is a different one; for the console, use the OTG port.
- Use **Chrome or Edge** on a desktop: Firefox and Safari have no Web Serial.
- A snap or flatpak browser may not see serial ports at all; a packaged Chrome or Chromium does.

## The web console can't connect or keeps disconnecting

- **After a mode change** the dongle comes back as another USB device. The browser reconnects by
  itself once each mode has been picked in the Connect dialog once; until then, press Connect
  again and pick it.
- Only one program can hold the serial port: close `screen`, `idf.py monitor` or another
  console tab.
- **"None published here"** in the Update firmware card: the page was opened from a local file.
  Use [the hosted console](https://dinoyoshi.github.io/stadiacast-dongle/).

## A firmware update didn't stick

The dongle keeps the old firmware if anything goes wrong, so it's safe to try again.

- Keep the cable in for about half a minute, until the page reconnects.
- The new firmware is kept once the computer has the dongle's USB up for 10 s. If it restarted
  before that, or its servers didn't start, it went back to the old one by itself. The card
  says which version runs.
- **Install a .bin file** wants the app image (`stadiacast-dongle-firebeetle2-esp32p4.bin`, or
  `build/stadiacast_dongle.bin`), not the full-flash one.
- Firmware from before web console updates has one app slot: flash the full image over the
  programming port once ([Build and flash](../README.md#build-and-flash)).
