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
table; delete it and let `sdkconfig.defaults` apply.

**Without installing ESP-IDF**, flash a single merged image (made with `idf.py merge-bin`) at
offset 0: `esptool --chip esp32p4 write_flash 0x0 stadiacast-dongle.bin`, or in the browser with
[ESP Web Tool](https://espressif.github.io/esptool-js/) (address 0x0, Chrome or Edge).

### Settings

`idf.py menuconfig` → *Stadiacast dongle*:

| setting | default |
|---|---|
| hotspot name / password / channel | `stadiacast-dongle` / `stadiacast` / 6 |
| USB headphones (off: gamepad only) | on |
| Opus encoder complexity | 8 |
| controller mode at the first power-up | Xbox 360 |
| status LED GPIO (-1: none) | 3 |

## First-time controller setup

The controller remembers one Wi-Fi network, and it learns it over Bluetooth in its setup mode.
That happens once, from any PC with Bluetooth, with the Python package's provisioning tool:

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

Assistant is also push to talk for the headset microphone (below).
| L2 / R2 | analog triggers | analog + digital | ZL / ZR (digital) | analog axes |

Sticks are passed through raw (no deadzone, as a real pad).

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

## Known limits

- Some loud songs clip a little in the headset; a few dB of headroom before the Opus encoder is
  the likely fix.
- Rumble is tested in Xbox 360 mode. DualShock 4 and Switch Pro rumble, and the Switch Pro mode
  on a real Switch console, are untested.
- The headset microphone is tested on Linux only. While it runs, the headphones' stream has a
  few more underruns (3 to 6 per 10 s instead of 0 to 1): the P4 is close to its limit encoding
  and decoding at once.
- One controller at a time (the Python receiver handles several).
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
  USB (iso OUT every microframe) -> 20 ms frames -> Opus 96 kbps (libopus, fixed point)
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

### If the Wi-Fi doesn't come up

The P4 reaches the C6 over SDIO (D0 to D3 on GPIO14 to 17, CLK on GPIO18, CMD on GPIO19, C6
reset on GPIO54, from the FireBeetle wiki). The C6 has to run Espressif's **ESP-Hosted slave
firmware**. If the log shows SDIO or "slave" errors, or a version mismatch warning from
`esp_hosted`, update the C6 as above or check the pins.

If the board boot-loops before `app_main` with an assert in `vApplicationGetIdleTaskMemory`, the
static data has grown too big: on v1.x P4 chips the heap before the scheduler starts is only the
RAM after `.bss` (about 100 KB). Keep large buffers off `.bss` and allocate them at run time.
