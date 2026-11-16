# Stadiacast dongle firmware (FireBeetle 2 ESP32-P4)

A port of [Stadiacast](https://github.com/jkfran/stadiacast) to the DFRobot FireBeetle 2
ESP32-P4 (DFR1172), which turns the board into a **USB dongle for the Stadia Controller**. The
controller joins a Wi-Fi hotspot run by the board's ESP32-C6, the P4 plays the parts of Google's
servers the controller talks to, and the board shows up on the PC, console or phone as a wired
USB gamepad with rumble and USB headphones. No PC software, no drivers, no cloud.

The Python receiver at the top of this repository stays the reference implementation; the
protocol work behind both is jkfran's. Where this port stands, what was tested and what is next
is in [docs/progress.md](docs/progress.md).

## What works

| feature | status |
|---|---|
| Controller joins the board's hotspot, HTTPS boot endpoints, WebRTC signalling, DTLS, READY | tested on the board |
| USB gamepad: every button, stick and trigger | tested on Linux and Windows |
| Rumble from the game to the controller's motors | tested in Xbox 360 mode (Linux) |
| USB headphones: the PC's sound plays on the controller's 3.5 mm jack (Opus) | tested on Linux, and on Windows in Xbox 360, DualShock 4 and HID modes |
| Four controller modes: Xbox 360, DualShock 4, Switch Pro, HID gamepad | tested on Linux (Steam sees DS4 mode as a PS4 controller) |
| Runs from the OTG USB-C port alone, LED shows its state | tested on the board |
| Updates the C6's ESP-Hosted firmware from the P4 | done once: 2.12.11 to 2.13.1 |
| Headset microphone: the headset on the controller is the PC's USB microphone, push to talk on the Assistant button | tested on Linux (PipeWire) |
| Web console in the browser over the dongle's USB cable: live stats, settings, rumble test | tested on the board (Linux) |
| Controller battery and connection quality in the web console | battery tested on the board |
| Firmware updates from the web console, with rollback | tested on the board |
| Pairing a controller from the web console over the board's own Bluetooth, no PC tools | tested on the board |
| Several controllers at once (up to 4), one USB gamepad each in Xbox 360 and HID modes | tested on a PC with two fake controllers; on the board with one real controller only |

The controller needs its **Wi-Fi firmware** (ours runs build 316382; the Python receiver is
tested with `ota.282115`). See [Known limits](#known-limits) for what is rough.

## Hardware

- A **DFRobot FireBeetle 2 ESP32-P4** (DFR1172): ESP32-P4 with 16 MB flash and 32 MB PSRAM, and
  an ESP32-C6 for Wi-Fi, wired to the P4 over SDIO.
- Two USB-C ports. The **high-speed OTG port** is the dongle: power, gamepad and headphones. The
  other one (serial / programming) is only for flashing and the log.
- Nothing else: the status LED (GPIO3) and the BOOT button (GPIO35) are on the board.

**Chip revision.** FireBeetle boards ship with early P4 silicon (v1.x; ours is v1.3), and an
ESP-IDF build made for v3.x chips does not boot on them. `sdkconfig.defaults` already selects
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`. On a board with a v3 chip, remove the two `REV` lines
there.

## Build and flash

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32p4/get-started/index.html)
(the VS Code extension installs it in a few clicks).

```sh
git submodule update --init       # once: libopus, in components/opus/libopus
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash monitor     # the serial / programming USB-C port, not the OTG one
```

The first build downloads `esp_hosted`, `esp_wifi_remote` and `esp_tinyusb` from the Espressif
component registry. An existing `sdkconfig` from an old checkout may predate the custom partition
table or newer settings (such as TLS buffers in PSRAM); delete it and let `sdkconfig.defaults`
apply.

**Without installing ESP-IDF**, flash the whole-flash image that CI publishes with the web
console, [stadiacast-dongle-full.bin](https://dinoyoshi.github.io/stadiacast/firmware/stadiacast-dongle-full.bin)
(or one you made with `idf.py merge-bin`), at offset 0 over the programming port:
`esptool --chip esp32p4 write_flash 0x0 stadiacast-dongle-full.bin`, or in the browser with
[ESP Web Tool](https://espressif.github.io/esptool-js/) (address 0x0, Chrome or Edge). After
that, update from the web console (below), with no tools and over the dongle's own cable.

**Coming from a firmware before web console updates** (the flash layout with one `factory`
app): flash over the programming port once more, with `idf.py flash` or the full image above.
That writes the new layout with two app slots and a bootloader that can roll back. The settings
and the C6 firmware stay where they are. Delete an old `sdkconfig` first so the rollback option in
`sdkconfig.defaults` applies.

### Settings

`idf.py menuconfig` → *Stadiacast dongle*:

| setting | default |
|---|---|
| hotspot name / password / channel | `stadiacast-dongle` / `stadiacast` / 6 |
| USB headphones (off: gamepad only) | on |
| Opus encoder complexity | 8 |
| Opus encoder complexity while the microphone runs (when lower) | 4 |
| Opus bitrate (kbit/s, 32-160) | 96 |
| controller mode at the first power-up | Xbox 360 |
| status LED GPIO (-1: none) | 3 |
| USB serial console for the web console | on |

The headphone gain, Opus complexity and bitrate, microphone, push to talk and rumble can also be changed at run time
from the web console (below); what it sets is saved on the board and wins over menuconfig.

## First-time controller setup

The controller remembers one Wi-Fi network, and it learns it over Bluetooth in its setup mode.
That happens once per controller. The dongle can do it itself, through the C6's Bluetooth radio:

1. Put the controller in setup mode: turn it off (hold Stadia until the light goes out), then
   hold Y and press Stadia. The light pulses orange. **This replaces the network the controller
   knows**: afterwards it only joins the dongle (pair it again to change that).
2. In the [web console](#web-console), Controllers card, press **Pair a controller** (or send
   `pair` from a terminal). The dongle finds the controller, reads its key, and gives it the
   hotspot's name and password, encrypted for that controller. The card follows each step.
3. The controller joins the hotspot. In our test it connected by itself about 20 s after
   pairing. If it doesn't (its first start after setup can skip the session, see
   `docs/protocol.md` section 11), wait a minute, turn it off (hold Stadia 4 s) and wake it.
   From then on it connects like any other wake.

Or from any PC with Bluetooth, with the Python package's provisioning tool:

```sh
pip install 'stadiacast[ble]'          # or a release executable of Stadiacast
# controller off, then hold Y and press Stadia: orange light = setup mode
stadiacast-provision scan
stadiacast-provision provision --address <ADDR> --ssid stadiacast-dongle --password stadiacast
```

Then power the dongle and wake the controller. The first boot after provisioning may not
connect: wait a minute, turn the controller off (hold Stadia 4 s) and wake it again. A PC hotspot
with the same name and password works as a fallback for the Python receiver, so the controller
never needs setting up twice ([docs/step1-baseline.md](docs/step1-baseline.md) has the details).

## Using it

Once flashed, the dongle needs only the **OTG USB-C port**.

1. Plug the dongle into the PC, console or phone. The LED blinks fast while it starts.
2. When it blinks slowly (about 5 s after power-up), wake the controller with a ~1 s press of the
   Stadia button.
3. The LED stays on while the controller is connected: buttons, rumble and headset audio work.

When the controller sleeps or its battery dies, the board closes the session after 10 s of
silence and the gamepad goes back to neutral (LED back to slow blinking), so nothing stays held.
Waking the controller again reconnects it.

### Several controllers

The dongle serves up to four controllers at once. Set how many in the web console's
Controllers card (**Players at once**, or `set players N`), then restart the dongle. Every
controller has to be set up for the dongle's hotspot (above).

- Each controller gets a **player number** the first time it connects and keeps it, also
  across restarts (saved on the board). A new controller takes a free number; if all are taken,
  the one not heard from the longest. **Forget** in the console frees a number.
- In Xbox 360 and HID modes the PC sees one gamepad per player (player 1 first), each with its
  own rumble. DualShock 4 and Switch Pro modes stay one gamepad, player 1's: the others connect
  but their input goes nowhere.
- Headset audio and the microphone are player 1's. The mode combos (Stadia + A/B/X/Y) work on
  player 1's controller.
- The LED stays on while any controller is connected.
- With more than one player in Xbox 360 mode the board uses the same USB ID as with the
  headphones (1209:0001), so Windows gives each gamepad its own Xbox driver.

### Controller modes

The dongle can be four kinds of USB controller. Hold the **Stadia button with a face button for
1.5 s** (not 4 s: that turns the controller off). The board leaves USB, comes back as the new
controller, and pulses the controller's motors once per mode number. A press of the board's
**BOOT button** steps to the next mode instead, with or without the controller. Either way the
LED blinks the mode number, and the board remembers the choice across power-ups.

| combo | mode | USB ID | for | rumble | headphones |
|---|---|---|---|---|---|
| Stadia + A | Xbox 360 (XInput), the default | 045E:028E, or 1209:0001 with the headphones on | Windows, Linux, SteamOS, Android | yes | yes |
| Stadia + B | DualShock 4 | 054C:09CC | PC games and Steam with PlayStation prompts; not a real PS4 (it checks for Sony hardware) | yes | yes |
| Stadia + X | Switch Pro Controller | 057E:2009 | Nintendo Switch (wired), Steam, Linux | yes | no |
| Stadia + Y | HID gamepad | 303A:4047 | macOS, Android, older DirectInput games | no | yes |

On the Switch, turn on *System Settings → Controllers and Sensors → Pro Controller Wired
Communication*.

### Button mapping

Face buttons map by position: the bottom one (Stadia A) is Xbox A, PlayStation cross and
Nintendo B.

| Stadia | Xbox 360 | DualShock 4 | Switch Pro | HID |
|---|---|---|---|---|
| Stadia | Guide | PS | Home | button 9 |
| Menu (≡) | Start | Options | + | button 8 |
| Options (⋯) | Back | Share | − | button 7 |
| Capture | (none) | touchpad click | Capture | button 12 |
| Assistant | (none) | (none) | (none) | button 13 |
| L2 / R2 | analog triggers | analog + digital | ZL / ZR (digital) | analog axes |

Sticks are passed through raw (no deadzone, as a real pad). Assistant is also push to talk for
the headset microphone (below).

### Headset audio

Pick the dongle as the sound output and plug headphones into the controller. It is a USB Audio
Class 2.0 device (48 kHz, 16-bit stereo, with volume and mute), which Windows 10+, macOS, Linux
and Android drive without anything to install.

- **Linux** names the output after the gamepad ("Xbox 360 Controller Analog Stereo" in Xbox
  mode). Set it to 100%: the volume is applied on the board.
- **Windows:** in Xbox 360 mode with the headphones on, the dongle is 1209:0001 so that Windows'
  Xbox driver does not take the whole device. Games still see an XInput pad ("Xbox Pad" in
  Steam), and the headphones get the standard USB audio driver. With the headphones turned off
  in menuconfig the dongle stays 045E:028E.

## Web console

A page that shows what the dongle is doing and changes its settings, over USB: plug the dongle
into the computer, open the console in Chrome or Edge (desktop) and press **Connect**.

### Using the console, step by step

1. **Connect.** Plug the dongle's OTG port into the computer and open
   [dinoyoshi.github.io/stadiacast](https://dinoyoshi.github.io/stadiacast/) in Chrome or Edge.
   Press **Connect** and pick the dongle's serial port in the browser's list (on Linux, the one
   named after the gamepad, usually `/dev/ttyACM*`). The cards fill in within a second. On Linux
   your user needs to be in the `dialout` group (`uucp` on Arch) first.
2. **Settings.** In the Settings card, a change applies as soon as you pick it and is saved on
   the dongle: gamepad mode (the dongle leaves USB and comes back as the new controller; the page
   reconnects), headphone gain, Opus complexity and bitrate, push to talk. The microphone switch
   and the number of players take effect after **Restart dongle**. **Reset settings** goes back
   to the build's defaults. Rumble on/off and strength are in the Rumble card, with test buttons.
3. **Install latest.** In the Update firmware card, compare the running version with the
   published one and press **Install latest** when they differ. Keep the cable in for about half
   a minute; the dongle restarts on the new firmware and the page reconnects. If anything goes
   wrong, the dongle goes back to the old firmware by itself (details below).
4. **Pair.** To set up a controller for the dongle, put it in setup mode (off, then hold Y and
   press Stadia: the light pulses orange), then press **Pair a controller** in the Controllers
   card and confirm. The card shows each step; the controller then joins the dongle's hotspot.
   See [First-time controller setup](#first-time-controller-setup).

### What the cards show

- Hosted: `https://dinoyoshi.github.io/stadiacast/` (once GitHub Pages is on, see below), or
  open `console/index.html` from a checkout. Add `?demo` to look around without a dongle.
- Live: controller session, its battery and connection quality (below), headphone frames, bitrate, underruns and encode
  time, microphone packets and level, CPU per core, each task's CPU share (paused while the headphones play; Refresh lists them once), free memory, the
  controller's input, and the firmware's log.
- Settings: gamepad mode, headphone gain (-12 to 0 dB before the Opus encoder), Opus complexity,
  Opus complexity while the mic is on, Opus bitrate (32 to 160 kbps), microphone (after a restart), push to talk; restart and reset buttons.
- Controllers: each player's controller (id, address, status, battery, round trip, input
  lost), **Forget**, the number of players, and **Pair a controller** (above).
- Rumble: on/off and a strength per motor (0 to 200%; 100% is what the game asks for, more lifts
  weak rumble), the levels the motors get right now, and test buttons (left, right, both,
  gentle, stop) that run the motors for a second through those settings.

The controller card's **Battery** is the controller's own gauge. Its first reading arrives a few
minutes after the controller connects and then once per percent it drops, so the page shows
when the latest one came. The dongle keeps it across reconnects until it restarts.
**Connection** grades the link Good, Fair or Poor by the worst of three figures:

- Wi-Fi signal: the controller's RSSI at the hotspot (good from -60 dBm, fair from -72 dBm).
- Round trip: the time the controller takes to answer the dongle's keepalive check, sent every
  2.5 s on the game link (good up to 20 ms, fair up to 60 ms).
- Input lost: the controller numbers its input packets, so a skipped number is a packet that
  never arrived. Shown for the last minute (good under 1%, fair under 5%), with any keepalive
  checks that went unanswered.

The dongle carries a USB serial port for it in every mode (`/dev/ttyACM*` on Linux, a COM port
on Windows, no driver needed). The page uses Web Serial, which Firefox and Safari don't have. On
Linux your user needs to be in the `dialout` group (`uucp` on Arch). After a mode change the
dongle comes back as another USB device: the browser reconnects by itself once each mode has
been picked in the Connect dialog once.

### Updating the firmware from the web console

The **Update firmware** card installs new firmware over the same USB cable: no ESP-IDF, no
esptool, no BOOT button.

- **Install latest** takes the build that CI publishes next to the page
  (`firmware/stadiacast-dongle.bin`, built from `main` with ESP-IDF 5.5.6 and the defaults in
  `sdkconfig.defaults`). The card shows which version runs and which is published.
- **Install a .bin file** takes your own build: `build/stadiacast_dongle.bin` (the app image,
  not the merged full-flash one).

It takes about half a minute. The image goes to the slot that isn't running, and the dongle only
switches once all of it is there, its SHA-256 matches, and ESP-IDF accepts it for this chip and
revision; anything else leaves the old firmware running. The new firmware then starts **on
trial**: it is kept once the computer has the dongle's USB up and 10 s have passed. If it
crashes or restarts before that, the bootloader goes back to the old one by itself, and if the
controller's servers don't start, the new firmware goes back to the old one at once.

The published build uses the menuconfig defaults (hotspot name and password, LED pin and so on);
the settings the console changes are in NVS and carry over. A board with a v3 P4 needs its own
build. The page only has the published build when it comes from GitHub Pages, not from a local
`index.html`.

### From a terminal

The same port works from a terminal (`screen /dev/ttyACM0`): type `hello`, `get`,
`set gain -3`, `set complexity 5`, `set mic_complexity 3`, `set bitrate 64`, `set mode 1`, `set rumble 0`,
`set rumble_left 150`, `rumble 255 255 1000` (a 1 s test, levels 0-255 per motor), `logs on`, `reboot`. The protocol is described
at the top of `main/console.c` (and the firmware update's at the top of `main/ota.c`).

GitHub Pages: in the repository's Settings → Pages, set Source to "GitHub Actions". The
`Web console and firmware (GitHub Pages)` workflow then builds the firmware and publishes it with
`console/` on every change under `firmware/esp32p4/` on `main` (pull requests only build it).

## Known limits

- Some loud songs clip a little in the headset. Lowering the headphone gain in the web console
  a few dB is the likely fix; not yet tried.
- The Opus bitrate setting works, but nobody has compared bitrates by ear yet; 96 kbps is the
  default.
- Rumble is tested in Xbox 360 mode. DualShock 4 and Switch Pro rumble, and the Switch Pro mode
  on a real Switch console, are untested.
- The headset microphone is tested on Linux only; Windows is untested.
- Keep the Opus complexity at 8 or below: 10 stutters (encoding takes up to 15 ms of each
  20 ms frame). While the mic runs the encoder drops to complexity 4 by itself.
- Several controllers are tested on a PC build (two fake controllers) and on the board with one
  real controller; players 2 to 4 with real controllers are untested. Pairing over the board's
  Bluetooth is tested on the board with one controller.
- The controller needs its Wi-Fi firmware; a Bluetooth-mode controller has to be flashed back
  first ([docs/step1-baseline.md](docs/step1-baseline.md)).

---

## For developers

How the code is laid out:

| path | what |
|---|---|
| `main/` | start-up, the hotspot and DNS sink (`hotspot.c`), C6 update, mode switching, status LED |
| `components/stadia/` | the receiver: HTTPS, GMsg signalling, ICE/DTLS-SRTP, display and game sessions, Opus audio; plain C, also builds for Linux |
| `components/usb_pad/` | TinyUSB device: XInput, DualShock 4, Switch Pro and HID descriptors and reports, the UAC2 headphones |
| `components/opus/` | libopus 1.5.2 (git submodule), fixed point |
| `host_test/` | the Linux build of `components/stadia` for testing against the fake controller |
| `c6_fw/` | where the C6's `network_adapter.bin` goes for an update (not committed) |

### What the serial log should show

Waking the controller, the board's log goes through the hotspot, HTTPS, signalling, DTLS and
READY, then the game session and the audio route:

```
hotspot: hotspot up: ssid "stadiacast-dongle", channel 6, board at 192.168.4.1
dongle: ready: wake the controller (about 1 s press of the Stadia button)
hotspot: station joined: xx:xx:xx:xx:xx:xx (aid 1)
hotspot: dhcp: xx:xx:xx:xx:xx:xx got 192.168.4.2
dns: A cloudcast-pa.googleapis.com from 192.168.4.2 -> board
https: GET connectivitycheck.gstatic.com/generate_204 -> 204 netcheck
https: POST cloudcast-pa.googleapis.com/v1:FirmwareVersionCheck -> 200
receiver: RegisterController from <id> (attempt ...) with a resumed discovery and offer
signaling: offer ...: raw_dtls=1 delayed_acks=1, 1 IPv4 candidate(s); answering with 192.168.4.1:40000
receiver: GMsg #1 delivered to stadia-local-receiver-display-0
rtc: display: DTLS connected: TLS-ECDHE-ECDSA-..., SRTP AES128_CM_SHA1_80 keys exported, peer fingerprint verified
receiver: ResolveDiscovery: discovery complete
rtc: controller: <product>, hardware <hw>, firmware <fw>, headset out
rtc: display state: ready
rtc: SetAudioMode(WebRTC Opus) sent
rtc: StartGame local-... sent
game: InitializationRequest v1 (raw_dtls=1): answering with 192.168.4.1:40001
game: game session connected
game: input: A+ L(0,0) R(0,0) L2 0 R2 0
game: rumble left 65535 right 32896
audio: headset audio route open: sending
usb_audio: the host started playing
audio: 500 frames sent in 10 s (160 B avg), 0 not sent, 0 trimmed, 0 underruns, 0 resyncs; encode avg ... us, max ... us
```

The GMsg poll after the ack is held for 20 s, so ResolveDiscovery and READY can come up to 20 s
after DTLS connects, the same as the Python receiver.

### How the pieces fit

- **Input.** Input goes to the USB host as soon as the controller is READY. A second later the
  board sends StartGame, the controller opens the game session on UDP 40001, and from then on
  input comes from there and the host's rumble goes back to the motors (refreshed every 2 s,
  ahead of the controller's 5 s haptics watchdog).
- **Audio.** The device is composite: the gamepad interface plus a UAC2 sound card on the same
  OTG port.

  ```
  USB (iso OUT every microframe) -> 20 ms frames -> Opus 96 kbps by default (libopus, fixed point)
    -> RTP PT 111 -> SRTP (the display link's DTLS keys) -> UDP to the controller
  ```

  At READY the board sends `SetAudioModeRequest(mode 2)` (without it the controller drops the
  audio). Frames are encoded at the USB host's pace and sent on the board's own 20 ms grid; a
  pause leaves a gap of the same length in the RTP timestamps. The hotspot runs with a DTIM
  period of 1, so frames for a dozing controller are not held back.
- **Windows and Xbox mode.** Windows' Xbox driver (xusb22) claims 045E:028E for the whole device,
  which left the headphones without a driver. As 1209:0001, Windows' composite driver splits the
  device: the gamepad interface gets the Xbox driver through the `XUSB10` compatible ID in a
  Microsoft OS 2.0 descriptor, and the headphones get the USB Audio 2.0 driver. Linux's xpad
  drives 1209 XInput pads too (kernel 5.12+).
- **Microphone.** The same sound card has a microphone (48 kHz 16-bit mono, volume -50 to 0 dB
  and mute), fed by the headset plugged into the controller. It is **push to talk**: hold the
  Assistant button to talk. The board asks the controller for its microphone when the button
  goes down (sound arrives a frame or two later) and tells it to stop 0.3 s after it comes up,
  so the end of a word is kept; anything the controller still sends then is dropped, and the
  computer hears silence. menuconfig → *Push to talk* off keeps the microphone always on.

  ```
  DisplayToController #5 BoolValue(true) -> the controller sends Opus (PT 111, 20 ms, DTX)
    as SRTP on the display link's pair -> checked and decrypted (the controller's DTLS keys)
    -> Opus decoder (mono, 48 kHz; lost frames concealed) -> 40 ms buffer -> USB IN 0x82
  ```

  Nothing documented that request: a probe build sent each display request whose use was not
  known, and #5 with `true` started the stream 24 ms later (controller firmware 316382). The
  stream is under the display offer's audio SSRC and uses DTX (about 7 tiny packets a second
  while nobody talks). It stopped by itself once during that test, so the board repeats the
  request whenever nothing has come for a while: 1 s while Assistant is held, 3 s with push to
  talk off (then the first request goes out after each link's SetAudioMode answer). The
  buffer follows the computer's clock: past 120 ms of backlog it skips back to 40 ms, and when it
  runs dry the computer gets silence until it has 40 ms again. menuconfig → *Pass the
  controller's microphone to the USB headset* turns the request and the decoder off.

  What the other unknown requests answered, for the record: #6, #7, #11, #14, #17 and #18 (float)
  OK; #13, #22 and #23 UNIMPLEMENTED; #10 with an empty string code 1 "Disconnected before
  connect completed."; `SetAudioMode` mode 1 "Audio mode not implemented", mode 4 OK, and mode 2
  with `f2` INVALID_ARGUMENT.

### Updating the C6 (Wi-Fi) firmware

The C6 runs the ESP-Hosted slave firmware it came with (2.12 on our board), while the P4 is
built with the 2.13 host side. The board can bring the C6 up to the P4's version itself, over
the same SDIO link (no extra wiring):

1. Build `network_adapter.bin` for the C6 and put it in `c6_fw/` ([c6_fw/README.md](c6_fw/README.md)).
2. `idf.py build flash`: the image goes to the P4's `c6_fw` partition along with the app. If this
   build folder was configured before the .bin was there, run `idf.py reconfigure` first (the
   build prints "C6 firmware: ... goes to the c6_fw partition" when it is picked up).
3. At boot the P4 compares versions and, if they differ, updates the C6, then restarts:

```
c6_update: updating the C6 from ESP-Hosted 2.12.0 to 2.13.1 (... bytes); keep the board powered
c6_update: 10% ... 100%
c6_update: C6 updated to 2.13.1: restarting so both sides start together
...
c6_update: C6 runs ESP-Hosted 2.13.1, as on the board
```

The C6 keeps its old firmware until the new one is written and verified, so an interrupted
update leaves it working. The partition table holds a 3 MB app and a 2 MB `c6_fw` partition.

### Testing on a PC

`components/stadia/` is plain C on BSD sockets and mbedTLS, so it also builds for Linux and
runs against the Python fake controller in `tests/fake_controller.py`:

```sh
cmake -S host_test -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls
cmake --build build-host
STADIA_ALLOW_CLEARCUT=1 build-host/stadia_host 127.0.0.1 8443 40000 &
uv run tests/fake_controller.py --host 127.0.0.1 --https-port 8443   # from the repository root
```

For the game session, start the host build with `STADIA_START_GAME=1` and run the fake
controller with `--game 12`. The host build turns pressing A into rumble and stops it 6 s after
A is released, so the fake controller shows the motors starting, staying on through the 5 s
haptics watchdog (our 2 s refreshes), and stopping.

With `STADIA_AUDIO=1` the host build also opens the headset audio route and plays a 440 Hz tone;
the fake controller (`--game`) ends with a line on what its jack got: frames played (SRTP
checked), their SSRC, sequence gaps and timestamp steps (all 960). The host build compiles the
same libopus as the board.

With `STADIA_MIC=1` as well, and `--mic-echo` on the fake controller, the fake controller sends
every frame it plays back as its own SRTP stream, the host build decodes it as the microphone,
and prints the tone it hears every 5 s (`microphone 5.0 s, 440 Hz, peak 8310`). Add
`STADIA_MIC_PTT=1` and `--talk 4` for push to talk: the fake controller holds Assistant for 4 s,
and the host build should decode about 4.3 s of tone and log `push to talk: talking`, then
`microphone off (DisplayToController #5 false) sent`.

Several controllers: start the host build with `STADIA_PLAYERS=2` and run two fake controllers,
each with its own `--device-id` (`--device-id ctl-one`, `--device-id ctl-two`). The host build
prints each player once a second (`player 2 known=1 id=ctl-two ... state=input`), and pressing A
on one rumbles only that one. `STADIA_KNOWN=ctl-two,ctl-one` gives the players remembered ids, as
the board's NVS does: `ctl-one` then becomes player 2.

`STADIA_STATS=1` prints the battery and link figures of the web console every second. With
`--game` the fake controller takes `--battery 0.76` (sends that gauge reading) and
`--input-gap 3` (skips three input sequence numbers once): the host build should then show
`battery=76%` and `gaps=3`.

### If the Wi-Fi doesn't come up

The P4 reaches the C6 over SDIO (D0 to D3 on GPIO14 to 17, CLK on GPIO18, CMD on GPIO19, C6
reset on GPIO54, from the FireBeetle wiki). The C6 has to run Espressif's **ESP-Hosted slave
firmware**. If the log shows SDIO or "slave" errors, or a version mismatch warning from
`esp_hosted`, update the C6 as above or check the pins.

If the board boot-loops before `app_main` with an assert in `vApplicationGetIdleTaskMemory`, the
static data has grown too big: on v1.x P4 chips the heap before the scheduler starts is only the
RAM after `.bss` (about 100 KB). Keep large buffers off `.bss` and allocate them at run time.
