# Stadiacast dongle firmware (FireBeetle 2 ESP32-P4)

A port of Stadiacast to the DFRobot FireBeetle 2 ESP32-P4 (DFR1172). The Stadia controller
joins a Wi-Fi hotspot run by the board's ESP32-C6, the P4 plays the parts of Google's servers
the controller talks to, and the board shows up as a USB gamepad with rumble and headset audio.
The Python code at the top of this repository stays as the reference implementation.

| step | what | status |
|---|---|---|
| 1 | PC baseline ([docs/step1-baseline.md](docs/step1-baseline.md)) and the C6 hotspot with a DNS sink | done: tested on the board, the controller joins and resolves through it |
| 2a | HTTPS with the built-in certificate, the boot endpoints, discovery RPCs and GMsg: the controller receives our WebRTC answer | done: tested on the board |
| 2b | STUN/DTLS on UDP 40000, display RPCs up to READY: input on the serial log | done: tested on the board, button names match |
| 3 | StartGame and the game session on UDP 40001; a USB Xbox 360 controller with rumble | done: tested on the board with Linux (xpad): all inputs, rumble held 8 s |
| 4 | USB headphones feeding the controller's headset jack (Opus over SRTP) | done: tested on the board with Linux (PipeWire): music plays in the controller's headset |
| 5 | USB microphone fed by the controller's headset microphone | experimental: the decode path is tested on a PC; how the controller sends its microphone is still being found out on the board |

## Build and flash

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32p4/get-started/index.html).
The extension for VS Code installs it in a few clicks.

```sh
git submodule update --init       # once: libopus, in components/opus/libopus
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash monitor     # the USB-C port marked for programming / CDC, not the OTG one
```

The first build downloads `esp_hosted`, `esp_wifi_remote` and `esp_tinyusb` from the Espressif
component registry. `idf.py menuconfig` → *Stadiacast dongle* changes the hotspot's name, password and
channel. The defaults are `stadiacast-dongle` / `stadiacast` on channel 6. The same menu turns the
USB headphones off (gamepad only) and sets the Opus encoder's complexity (default 8).

## What step 1 should print

```
hotspot: hotspot up: ssid "stadiacast-dongle", channel 6, board at 192.168.4.1
dongle: ready: wake the controller (about 1 s press of the Stadia button)
hotspot: station joined: xx:xx:xx:xx:xx:xx (aid 1)
hotspot: dhcp: xx:xx:xx:xx:xx:xx got 192.168.4.2
dns: A connectivitycheck.gstatic.com from 192.168.4.2 -> board
dns: A cloudcast-pa.googleapis.com from 192.168.4.2 -> board
```

After that the controller tries HTTPS, which nothing answers yet, so it keeps blinking and
goes back to sleep. That's expected for step 1.

### Without installing ESP-IDF

Each step also comes as one ready-made image, `stadiacast-dongle-step1.bin`. Flash it at offset 0
with `esptool --chip esp32p4 write_flash 0x0 stadiacast-dongle-step1.bin`, or in the browser with
[ESP Web Tool](https://espressif.github.io/esptool-js/) (address 0x0, Chrome or Edge).
Then watch the log with any serial monitor at 115200 baud.

## What step 2 should print

After waking the controller, beyond the step 1 lines:

```
https: GET connectivitycheck.gstatic.com/generate_204 -> 204 netcheck
https: 192.168.4.2: refused the Clearcut upload (TLS alert handshake_failure)
https: POST cloudcast-pa.googleapis.com/v1:FirmwareVersionCheck -> 200
receiver: RegisterController from <id> (attempt ...) with a resumed discovery and offer
signaling: offer ...: raw_dtls=1 delayed_acks=1, 1 IPv4 candidate(s); answering with 192.168.4.1:40000
receiver: GMsg #1 delivered to stadia-local-receiver-display-0
receiver: GMsg #1 acked
rtc: display: ICE pair selected: 192.168.4.2:<port> (binding response)
rtc: display: DTLS connected: TLS-ECDHE-ECDSA-..., SRTP AES128_CM_SHA1_80 keys exported, peer fingerprint verified
receiver: ResolveDiscovery: discovery complete
rtc: controller: <product>, hardware <hw>, firmware <fw>, headset out
rtc: display state: ready
rtc: input: A+ L(0,0) R(0,0) L2 0 R2 0
```

The GMsg poll after the ack is held for 20 s, so ResolveDiscovery and READY can come up to
20 s after DTLS connects, the same as the Python receiver.

## Step 3: the USB gamepad

The board's **high-speed USB-C port (OTG)** is the gamepad: plug it into the PC, console or
phone. It shows up as a wired Xbox 360 controller (045E:028E, XInput), which Windows, Linux,
SteamOS and Android drive without anything to install. The other USB-C port stays the serial
console. Input goes to the USB host as soon as the controller is READY; a second after READY
the board sends StartGame, the controller opens the game session, and from then on its input
comes from there and the host's rumble goes back to the controller's motors.

```
usb_pad: Xbox 360 style gamepad on the high-speed USB-C port
usb_pad: XInput interface opened by the host
...
rtc: display state: ready
rtc: StartGame local-... sent
game: InitializationRequest v1 (raw_dtls=1): answering with 192.168.4.1:40001
rtc: game: DTLS connected: ...
game: game session connected
rtc: StartGame answered OK after ... ms: the game session is up
game: input framing: rtp
game: input: A+ L(0,0) R(0,0) L2 0 R2 0
game: rumble left 65535 right 32896
```

Mapping: the Stadia button is the Xbox (guide) button, Menu is Start, Options is Back. Assistant
and Capture have no XInput equivalent. Sticks are passed through raw (no deadzone, as a real
pad), triggers 0 to 255.

## Step 4: headset audio

The same OTG port is also **USB headphones** ("Stadiacast Dongle", 48 kHz 16-bit stereo, with
volume and mute), so the device is composite: the gamepad plus a USB Audio Class 2.0 sound card,
which Windows 10+, macOS, Linux and Android support without a driver. Whatever the computer
plays there goes to the controller's 3.5 mm headset jack:

```
USB (1 ms packets) -> 20 ms frames -> Opus 96 kbps (libopus, fixed point) -> RTP PT 111
  -> SRTP (the display link's DTLS keys) -> UDP to the controller, on the display link's pair
```

At READY the board sends `SetAudioModeRequest(mode 2)` (without it the controller drops the
audio), and once that is answered the frames go out. Pick "Stadiacast Dongle" as the output
device and plug headphones into the controller.

```
usb_pad: Xbox 360 style gamepad and USB headphones (48 kHz stereo) on the high-speed USB-C port
...
rtc: display state: ready
rtc: SetAudioMode(WebRTC Opus) sent
rtc: SetAudioMode answered: code 0
audio: headset audio route open: sending
usb_audio: the host started playing
audio: Opus libopus 1.5.2-fixed: 48 kHz stereo, 20 ms, 96 kbps, complexity 8, ... bytes of state
audio: 500 frames sent in 10 s (160 B avg), 0 not sent, 0 trimmed, 0 underruns, 0 resyncs; encode avg ... us, max ... us
```

The frames are encoded at the USB host's pace and sent on the board's own 20 ms grid, as
audio.py's pacer does; a pause leaves a gap of the same length in the RTP timestamps. The hotspot
runs with a DTIM period of 1, so frames for a dozing controller are not held back. On Linux,
set the dongle's output to 100%: its volume is applied on the board.

In Xbox 360 mode with the headphones on, the dongle is **1209:0001** rather than 045E:028E.
Windows' Xbox driver (xusb22) claims 045E:028E for the whole device, which left the headphones
without a driver on Windows. With another ID, Windows' composite driver splits the device: the
gamepad interface still gets the Xbox driver (XInput), through the `XUSB10` compatible ID in a
Microsoft OS 2.0 descriptor, and the headphones get the USB Audio 2.0 driver. Linux's xpad drives
1209 XInput pads too (kernel 5.12+). With the headphones turned off in menuconfig the dongle stays
045E:028E.

## Step 5: the microphone (experimental)

The USB headset also has a **microphone** (48 kHz 16-bit mono, with volume from -50 to +20 dB
and mute). Whatever the board decodes from the controller is recorded there:

```
UDP from the controller (either link) -> SRTP checked and decrypted (the controller's DTLS keys)
  -> RTP -> Opus decoder (mono, 48 kHz; lost frames concealed) -> 40 ms buffer -> USB IN 0x82
```

How the controller sends its microphone is not documented. The board assumes it is SRTP media
on a link's selected pair (the controller's WebRTC offers list an audio SSRC and Opus 48 kHz),
and logs what it sees so a run can tell:

```
signaling: offer audio: SSRC 00000457, codec 1, 2 channel(s), 48000 Hz
rtc: SetAudioMode(WebRTC Opus, f2 = microphone?) sent
rtc: SetAudioMode with f2 answered: code 0
rtc: display: media from the controller: SSRC ..., PT 111, seq ..., ... bytes
mic: microphone stream on the display link: PT 111, ...-byte packets, 960 samples each
mic: 500 packets in 10 s (display 500, game 0), 500 decoded, 0 lost, 0 errors, 0 dropped; 10000 ms of sound, peak -20 dBFS
usb_audio: the host started recording
usb_audio: microphone: 10000 ms in, 80000 packets out in 10 s, 0 underruns, 0 skips
```

`SetAudioModeRequest` has one bool field (`f2`) whose use is unknown; the board sets it as a
guess for "send the microphone too", and asks again without it if the controller refuses.
Both are menuconfig options (*Pass the controller's microphone to the USB headset*, *Ask for the
microphone in SetAudioMode*). With no "media from the controller" line, the controller sent
nothing: the microphone stays silent and the headphones work as before.

The buffer follows the computer's clock: past 120 ms of backlog it skips back to 40 ms, and when
it runs dry the computer gets silence until it has 40 ms again.

## Updating the C6 (Wi-Fi) firmware

The C6 runs the ESP-Hosted slave firmware it came with (2.12 on our board), while the P4 is
built with the 2.13 host side. The board can bring the C6 up to the P4's version itself, over
the same SDIO link (no extra wiring):

1. Build `network_adapter.bin` for the C6 and put it in `c6_fw/` ([c6_fw/README.md](c6_fw/README.md)).
2. `idf.py build flash`: the image goes to the P4's `c6_fw` partition along with the app. If this build folder was configured before the .bin was there, run `idf.py reconfigure` first (the build prints "C6 firmware: ... goes to the c6_fw partition" when it is picked up).
3. At boot the P4 compares versions and, if they differ, updates the C6 (about a minute), then
   restarts:

```
c6_update: updating the C6 from ESP-Hosted 2.12.0 to 2.13.1 (... bytes); keep the board powered
c6_update: 10% ... 100%
c6_update: C6 updated to 2.13.1: restarting so both sides start together
...
c6_update: C6 runs ESP-Hosted 2.13.1, as on the board
```

The C6 keeps its old firmware until the new one is written and verified, so an interrupted
update leaves it working. This changes the partition table (a 3 MB app, the 2 MB `c6_fw`
partition): an existing `sdkconfig` from an earlier step needs `CONFIG_PARTITION_TABLE_CUSTOM=y`
(or delete it and let `sdkconfig.defaults` apply).

## Using it on its own

Once flashed, the dongle needs only the **high-speed USB-C port (OTG)**: it powers the board and
carries the gamepad and the headphones. The programming port, the serial console and the PC
tools are only for flashing and debugging. The controller has to be provisioned for the
hotspot once ([docs/step1-baseline.md](docs/step1-baseline.md)); after that:

1. Plug the dongle into the PC, console or phone. The LED blinks fast while it starts.
2. When it blinks slowly (about 5 s after power-up), wake the controller with the Stadia button.
3. The LED stays on while the controller is connected: buttons, rumble and headset audio work.

When the controller sleeps or its battery dies, the board closes its session after 10 s of
silence and the gamepad goes back to neutral (and the LED back to slow blinking), so nothing
stays held. Waking the controller again reconnects it, as does unplugging and replugging the
dongle. The LED is GPIO3; `idf.py menuconfig` → *Stadiacast dongle* moves or turns it off.

## Controller modes

The dongle can be four kinds of USB controller. Hold the **Stadia button with a face button for
1.5 s** (not 4 s: that turns the controller off). The board leaves USB, comes back as the new
controller, and pulses the controller's motors once per mode number. A press of the board's
**BOOT button** steps to the next mode instead, with or without the controller. Either way the
board's LED blinks the mode number, and the board remembers the choice for the next power-up.

| combo | mode | for | rumble | headphones |
|---|---|---|---|---|
| Stadia + A | Xbox 360 (XInput; 045E:028E, or 1209:0001 with the headphones), the default | Windows, Linux, SteamOS, Android | yes | yes |
| Stadia + B | DualShock 4 (054C:09CC) | PC games and Steam with PlayStation prompts; not a real PS4, which checks for Sony hardware | yes | yes |
| Stadia + X | Switch Pro Controller (057E:2009) | Nintendo Switch (wired), Steam, Linux | yes | no |
| Stadia + Y | HID gamepad | macOS, Android, older DirectInput games | no | yes |

Buttons map by position: the bottom face button is Xbox A, PlayStation cross and Nintendo B.
Capture is the DualShock 4's touchpad click and the Switch's Capture button; the Stadia button is
Guide / PS / Home. On the Switch, turn on *System Settings → Controllers and Sensors → Pro
Controller Wired Communication*. `idf.py menuconfig` → *Stadiacast dongle* sets the mode for the
first power-up.

## Testing on a PC

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
and prints the tone it hears every 5 s (`microphone 5.0 s, 440 Hz, peak 8310`).

## If the Wi-Fi doesn't come up

The P4 reaches the C6 over SDIO (D0 to D3 on GPIO14 to 17, CLK on GPIO18, CMD on GPIO19, C6
reset on GPIO54, from the FireBeetle wiki). The C6 has to run Espressif's **ESP-Hosted slave
firmware**. If the log shows SDIO or "slave" errors, or a version mismatch warning from
`esp_hosted`, copy the first 50 lines of the log back to Claude. The fix is to update the C6
(ESP-Hosted can do that from the P4) or to adjust the pins.
