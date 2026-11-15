# Step 1: a known-good baseline

Before any code runs on the board, the controller has to work with the Python Stadiacast on a
PC. That proves the controller's firmware and its Wi-Fi setup, and gives us logs to compare
the board against in step 2.

## 1. Check the controller's firmware

The controller needs its **Wi-Fi firmware** (Stadiacast is tested with `ota.282115`). A
controller switched to Bluetooth mode can't be switched back from Google's site, because those
firmware files are gone.

- **Still on Wi-Fi firmware?** A Bluetooth-mode controller pairs over Bluetooth as
  "StadiaXXXX-XXXX". A Wi-Fi one doesn't. Instead it blinks orange in setup mode, or blinks
  white while it looks for its network. If you're not sure, just continue: step 3 below fails
  quickly when the controller is on the wrong firmware.
- **Need to go back?** [GaryOderNichts/StadiaController](https://github.com/GaryOderNichts/StadiaController)
  has `stadiatool`, which can dump and flash the controller's firmware over USB. To get into the
  bootloader: turn the controller off, unplug it, then hold **Options** while plugging it back in.
  Follow that repo's README for the flash itself, and for finding a Wi-Fi firmware image (it's
  a dump from a Wi-Fi controller). Make a dump of your current firmware first.

## 2. One network name for the PC and for the board

The controller remembers exactly one Wi-Fi network. Give the PC hotspot the **same name and
password as the board's hotspot**. Then the controller works with whichever one is switched on,
and it never has to be set up again:

| setting | value (the firmware's defaults, set in `idf.py menuconfig`) |
|---|---|
| SSID | `stadiacast-dongle` |
| password | `stadiacast` |
| security | WPA2-Personal (AES), protected management frames off, not WPA3 |

- **Windows:** Settings → Network & internet → Mobile hotspot, then edit the name, the
  password and the band (2.4 GHz).
- **macOS:** System Settings → General → Sharing → Internet Sharing, sharing over Wi-Fi.
- **Linux:** `nmcli dev wifi hotspot ssid stadiacast-dongle password stadiacast` (then stop
  dnsmasq's DNS on port 53, see the main `docs/setup.md`).

## 3. Run the Python Stadiacast

```sh
pip install 'stadiacast[ble,audio,gamepad]'      # or a release executable

# once: put the controller in setup mode (off, then hold Y and press Stadia: orange light)
stadiacast-provision scan
stadiacast-provision provision --address <ADDR> --ssid stadiacast-dongle

# every run (the router address is your PC's normal DNS server)
stadiacast --preset live --dns-upstream 192.168.1.1 --start-game --gamepad vigem   # Windows
stadiacast --preset live --dns-upstream 192.168.1.1 --start-game --gamepad ojd     # macOS
```

The first boot after provisioning never connects. Wait a minute, turn the controller off and
wake it again (see "What to expect on the first boot" in the main `docs/setup.md`).

## 4. Check the three things the dongle must do

With `<pc>` as the PC's hotspot address (`192.168.2.1` on macOS, `192.168.137.1` on Windows):

- [ ] **Input:** two short rumbles and a steady light, and each button press prints a line.
      `curl http://<pc>/_input` shows what's held.
- [ ] **Rumble:** `curl -d '{"left":40000,"right":40000,"ms":1500}' http://<pc>/_rumble`
- [ ] **Headset audio:** plug headphones into the controller, then
      `curl -d '{"do":"start","source":"system"}' http://<pc>/_audio` and play something.
      This needs ffmpeg with libopus on `PATH`.

## 5. Keep the logs

Every run writes a folder `captures/<stamp>/` in the directory you started it from (`--log-dir` picks another place). Do all three checks in one run. Don't commit the folder. It holds your controller's serial
number. Leave it on your computer and tell Claude where it is, and Claude reads it from there.

## 6. Flash the board's step 1 build

See `../README.md`. With the PC hotspot off and the board powered, wake the controller. The
board's serial log should show it join, get an address and ask for the Google names. It stops
there, because the HTTPS part is step 2.
