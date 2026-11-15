# Stadiacast dongle firmware (FireBeetle 2 ESP32-P4)

A port of Stadiacast to the DFRobot FireBeetle 2 ESP32-P4 (DFR1172). The Stadia controller
joins a Wi-Fi hotspot run by the board's ESP32-C6, the P4 plays the parts of Google's servers
the controller talks to, and the board shows up as a USB gamepad with rumble and headset audio.
The Python code at the top of this repository stays as the reference implementation.

| step | what | status |
|---|---|---|
| 1 | PC baseline ([docs/step1-baseline.md](docs/step1-baseline.md)) and the C6 hotspot with a DNS sink | done: tested on the board, the controller joins and resolves through it |
| 2 | HTTPS endpoints, GMsg, STUN/DTLS up to READY: input on the serial log | next |
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

## If the Wi-Fi doesn't come up

The P4 reaches the C6 over SDIO (D0 to D3 on GPIO14 to 17, CLK on GPIO18, CMD on GPIO19, C6
reset on GPIO54, from the FireBeetle wiki). The C6 has to run Espressif's **ESP-Hosted slave
firmware**. If the log shows SDIO or "slave" errors, or a version mismatch warning from
`esp_hosted`, copy the first 50 lines of the log back to Claude. The fix is to update the C6
(ESP-Hosted can do that from the P4) or to adjust the pins.
