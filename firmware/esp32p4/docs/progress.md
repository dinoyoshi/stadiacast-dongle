# Port progress: Stadiacast on the FireBeetle 2 ESP32-P4

Last updated 2026-11-16. The goal is a USB dongle for the Stadia Controller: the controller
joins the board's Wi-Fi hotspot, and the board is a USB gamepad with rumble and headset audio,
with no PC software. The [README](../README.md) is the user guide; this page is the log of how
we got here and what is left.

## Status

All of the original plan is done and tested on the board. The dongle works on its own from the
OTG port, as four kinds of controller, on Linux and Windows.

| area | state | tested on |
|---|---|---|
| Hotspot, DNS sink, HTTPS boot endpoints | done | board |
| GMsg signalling, ICE, DTLS-SRTP, display link to READY | done | board, and the host build vs `tests/fake_controller.py` |
| Game session, USB gamepad, rumble | done | Linux (xpad): all inputs, rumble held 8 s |
| Headset audio (USB headphones to the 3.5 mm jack) | done | Linux (PipeWire); Windows in Xbox 360, DS4 and HID modes |
| Standalone: power from the OTG port, LED status, link timeout | done | board |
| Controller modes: Xbox 360, DualShock 4, Switch Pro, HID | done | Linux; Steam sees DS4 as a PS4 controller |
| C6 ESP-Hosted update from the P4 | done | board: 2.12.11 to 2.13.1 |
| Headset microphone | next | |

Test rig: FireBeetle 2 ESP32-P4 with a v1.3 P4, C6 on ESP-Hosted 2.13.1, a Stadia Controller on
Wi-Fi firmware build 316382, a Linux PC and a Windows PC.

## Timeline

All merged into `main`.

| PR | what | notes |
|---|---|---|
| #1 | Step 1: C6 hotspot with a DNS sink | controller joins and resolves the Google names through the board |
| #2 | Step 2a: HTTPS with the built-in certificate, boot endpoints, discovery RPCs, GMsg | the controller receives our WebRTC answer |
| #3, #4 | Step 2b: STUN/DTLS on UDP 40000, display RPCs up to READY | input on the serial log, button names match |
| #5 | Step 3: StartGame, game session on UDP 40001, USB Xbox 360 controller with rumble | TinyUSB XInput on the high-speed OTG port |
| #6 | Step 4: USB headphones to the controller's headset jack | own UAC2 driver, libopus 1.5.2, Opus over SRTP |
| #7 | Step 4 fixes on the board | iso OUT every microframe, 20 ms send grid, DTIM 1, 96 kbps at complexity 8 |
| #8 | C6 firmware update from the P4 | new partition table: 3 MB app, 2 MB `c6_fw` |
| #9 | Standalone | OTG port only, LED on GPIO3, session closed after 10 s of silence |
| #10 | Controller modes | Stadia + A/B/X/Y for 1.5 s, or the BOOT button; saved in NVS |
| #11 | Windows headset audio | Xbox+audio as 1209:0001 with an MS OS 2.0 `XUSB10` descriptor; UAC2 feature unit controls per channel |

## Things we learned the hard way

- **P4 chip revision.** FireBeetle boards carry v1.x P4s. Builds for v3.x do not boot, so
  `sdkconfig.defaults` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`.
- **Startup RAM on v1.x.** Before the scheduler starts, the heap is only the RAM after `.bss`
  (about 100 KB). A 1.4 KB static buffer was enough to make the idle task's stack allocation
  fail and boot-loop the board. Large buffers live on the heap.
- **Big request headers.** The real controller's RegisterController carries about 6 KB of
  headers (its cast certificate), so the HTTPS server accepts up to 16 KB of headers.
- **High-speed isochronous OUT.** On the P4's DWC2 controller an iso OUT endpoint with
  bInterval 4 never completed; bInterval 1 (every microframe) works.
- **Scratchy audio** went away once frames were sent on a fixed 20 ms grid, the hotspot's DTIM
  period was set to 1, and the encoder state moved to internal RAM.
- **Windows and 045E:028E.** Windows' xusb22 driver claims that ID for the whole composite
  device, so the headphones got no driver. Another ID plus the `XUSB10` compatible ID splits the
  device properly. Windows also wants UAC2 feature-unit controls for every channel, where Linux
  takes master only.

## Known issues

- Slight clipping on some loud songs. Likely fix: a few dB of headroom before the Opus encoder.
- DualShock 4 and Switch Pro rumble, and Switch Pro mode on a real Switch, are untested.
- One controller at a time.

## Next

1. **Headset microphone:** the controller's mic as a USB microphone on the dongle.
2. Audio headroom for the clipping above.
3. Test the Switch Pro mode on a Switch, and rumble in DS4 and Switch modes.
