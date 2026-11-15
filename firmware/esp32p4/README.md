# Stadiacast dongle firmware (FireBeetle 2 ESP32-P4)

A port of Stadiacast to the DFRobot FireBeetle 2 ESP32-P4 (DFR1172). The Stadia controller
joins a Wi-Fi hotspot run by the board's ESP32-C6, the P4 plays the parts of Google's servers
the controller talks to, and the board shows up as a USB gamepad with rumble and headset audio.
The Python code at the top of this repository stays as the reference implementation.

| step | what | status |
|---|---|---|
| 1 | PC baseline ([docs/step1-baseline.md](docs/step1-baseline.md)) and the C6 hotspot with a DNS sink | done: tested on the board, the controller joins and resolves through it |
| 2a | HTTPS with the built-in certificate, the boot endpoints, discovery RPCs and GMsg: the controller receives our WebRTC answer | done on PC; board: fixed the 6 KB RegisterController header |
| 2b | STUN/DTLS on UDP 40000, display RPCs up to READY: input on the serial log | this build: passes the PC loopback test |
| 3 | USB gamepad (Xbox 360 style) and rumble | |
| 4 | USB sound card feeding the controller's headset jack | |

## Build and flash

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32p4/get-started/index.html).
The extension for VS Code installs it in a few clicks.

```sh
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash monitor     # the USB-C port marked for programming / CDC, not the OTG one
```

The first build downloads `esp_hosted` and `esp_wifi_remote` from the Espressif component
registry. `idf.py menuconfig` → *Stadiacast dongle* changes the hotspot's name, password and
channel. The defaults are `stadiacast-dongle` / `stadiacast` on channel 6.

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
rtc: ICE pair selected: 192.168.4.2:<port> (binding response)
rtc: DTLS connected: TLS-ECDHE-ECDSA-..., SRTP AES128_CM_SHA1_80 keys exported, peer fingerprint verified
receiver: ResolveDiscovery: discovery complete
rtc: controller: <product>, hardware <hw>, firmware <fw>, headset out
rtc: display state: ready
rtc: input: A+ L(0,0) R(0,0) L2 0 R2 0
```

The GMsg poll after the ack is held for 20 s, so ResolveDiscovery and READY come about 20 s
after DTLS connects, the same as the Python receiver.

## Testing on a PC

`components/stadia/` is plain C on BSD sockets and mbedTLS, so it also builds for Linux and
runs against the Python fake controller in `tests/fake_controller.py`:

```sh
cmake -S host_test -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls
cmake --build build-host
STADIA_ALLOW_CLEARCUT=1 build-host/stadia_host 127.0.0.1 8443 40000 &
uv run tests/fake_controller.py --host 127.0.0.1 --https-port 8443   # from the repository root
```

## If the Wi-Fi doesn't come up

The P4 reaches the C6 over SDIO (D0 to D3 on GPIO14 to 17, CLK on GPIO18, CMD on GPIO19, C6
reset on GPIO54, from the FireBeetle wiki). The C6 has to run Espressif's **ESP-Hosted slave
firmware**. If the log shows SDIO or "slave" errors, or a version mismatch warning from
`esp_hosted`, copy the first 50 lines of the log back to Claude. The fix is to update the C6
(ESP-Hosted can do that from the P4) or to adjust the pins.
