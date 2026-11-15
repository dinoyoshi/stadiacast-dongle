# Firmware for the on-board ESP32-C6

`network_adapter.bin` goes here (it is not committed): the ESP-Hosted slave firmware for the
C6, of the same version as the `espressif/esp_hosted` component the P4 is built with. When it
is present, `idf.py flash` also writes it to the P4's `c6_fw` partition, and at boot the P4
installs it on the C6 over SDIO if the C6 runs another version (`main/c6_update.c`). After that
the file can stay: the version check makes the next boots skip the update.

Build it from the component's own slave project (after one `idf.py build` of the dongle, which
downloads the component):

```sh
cp -r managed_components/espressif__esp_hosted/slave /tmp/c6_slave
cd /tmp/c6_slave
idf.py set-target esp32c6        # SDIO is the default transport for the C6
idf.py build
cp build/network_adapter.bin <repo>/firmware/esp32p4/c6_fw/
```
