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
USB headphones off (gamepad only) and sets the Opus encoder's complexity (default 5).

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
USB (1 ms packets) -> 20 ms frames -> Opus 64 kbps (libopus, fixed point) -> RTP PT 111
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
audio: Opus libopus 1.5.2-fixed: 48 kHz stereo, 20 ms, 64 kbps, complexity 5, ... bytes of state
audio: 500 frames sent in 10 s (160 B avg), 0 not sent, 0 trimmed, 0 underruns, 0 resyncs; encode avg ... us, max ... us
```

The frames are encoded at the USB host's pace and sent on the board's own 20 ms grid, as
audio.py's pacer does; a pause leaves a gap of the same length in the RTP timestamps. The hotspot
runs with a DTIM period of 1, so frames for a dozing controller are not held back. On Linux,
set the "Xbox 360 Controller" output to 100%: its volume is applied on the board.

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

## If the Wi-Fi doesn't come up

The P4 reaches the C6 over SDIO (D0 to D3 on GPIO14 to 17, CLK on GPIO18, CMD on GPIO19, C6
reset on GPIO54, from the FireBeetle wiki). The C6 has to run Espressif's **ESP-Hosted slave
firmware**. If the log shows SDIO or "slave" errors, or a version mismatch warning from
`esp_hosted`, copy the first 50 lines of the log back to Claude. The fix is to update the C6
(ESP-Hosted can do that from the P4) or to adjust the pins.
