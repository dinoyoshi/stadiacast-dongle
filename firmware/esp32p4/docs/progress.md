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
| Headset microphone (the controller's headset as the PC's USB microphone) | done | Linux (PipeWire): voice recorded, no clipping, 0 packets lost |
| Push to talk on the Assistant button | done | board: sound 15-50 ms after the press, stops 0.3 s after release; headphone underruns about 0 per 10 s while the mic is idle |
| Web console (live stats and settings over a USB serial port, Web Serial page on GitHub Pages) | built | firmware builds; page tested with its demo; board test pending |

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
| #13 | Headset microphone | `DisplayToController` #5 true starts the controller's Opus mic stream (found by probing); UAC2 capture on iso IN 0x82 |
| #14 | Push to talk on the Assistant button | #5 true on the press, #5 false 0.3 s after release; packets outside that are dropped |
| #15 | Web console | CDC ACM serial port next to the gamepad in every mode; JSON lines with stats once a second; settings for mode, headphone gain, Opus complexity, mic, push to talk; static page in `console/`, deployed to GitHub Pages |
| #16 | TLS and audio buffers in PSRAM, faster flash | `MBEDTLS_EXTERNAL_MEM_ALLOC` (boots and reaches ready on the board); audio rings and queues (about 55 KB) in PSRAM via `port_alloc_bulk()`, Opus state and audio stacks stay internal; flash at QIO 80 MHz instead of DIO 40 MHz (80 MHz needs minimum chip rev v1.0; boots on the board), aimed at the headphone underruns while the mic runs (board test pending). Running code from PSRAM (`SPIRAM_XIP_FROM_PSRAM`) was tried and dropped: it boot-loops a v1.3 P4 |

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
- **PSRAM and audio.** PSRAM is fine for TLS buffers and the audio rings and queues (plain
  copies, and the TinyUSB interrupt is not IRAM so it never runs with the cache off), but not for
  the Opus state or the audio task stacks: Opus in PSRAM made encoding slower. Running the code itself from PSRAM
  (`SPIRAM_XIP_FROM_PSRAM`) boot-loops a v1.3 P4: it adds about 12 KB of internal RAM use and the
  idle task's stack allocation fails before the scheduler starts (the startup RAM issue above).
- **Scratchy audio** went away once frames were sent on a fixed 20 ms grid, the hotspot's DTIM
  period was set to 1, and the encoder state moved to internal RAM.
- **Windows and 045E:028E.** Windows' xusb22 driver claims that ID for the whole composite
  device, so the headphones got no driver. Another ID plus the `XUSB10` compatible ID splits the
  device properly. Windows also wants UAC2 feature-unit controls for every channel, where Linux
  takes master only.
- **The controller's microphone** is not sent until the display asks with `DisplayToController`
  #5 (a BoolValue) set to true; no document named it, a probe of the unknown requests found it.
  The f2 flag of SetAudioMode is not it (INVALID_ARGUMENT). The mic's 13 KB of static buffers
  boot-looped the board again (startup RAM, above), so they are on the heap too.

## Known issues

- Slight clipping on some loud songs. Likely fix: a few dB of headroom before the Opus encoder.
- DualShock 4 and Switch Pro rumble, and Switch Pro mode on a real Switch, are untested.
- One controller at a time.
- While Assistant is held (the microphone running), the headphones' stream has 1 to 3 underruns per 10 s (about 0 otherwise).

## Next

1. Audio headroom for the clipping above, and fewer headphone underruns while the mic runs:
   the web console's gain and Opus complexity settings make both easy to try on the board.
2. Try the microphone on Windows.
3. Test the Switch Pro mode on a Switch, and rumble in DS4 and Switch modes.
