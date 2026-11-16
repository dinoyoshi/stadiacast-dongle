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
| Web console (live stats and settings over a USB serial port, Web Serial page on GitHub Pages) | done | board over USB on Linux (2026-11-16) |
| TLS and audio buffers in PSRAM, flash at QIO 80 MHz | done | board (2026-11-16): boots, ready, mic works |
| Adjustable Opus bitrate (32-160 kbps) | done | host build; board listening test pending |
| Rumble settings and test from the console | done | board (2026-11-16) |
| Controller battery and connection quality in the console | done | board (2026-11-16): battery reading right; connection figures not yet checked on a weak link |
| Several controllers at once, one USB gamepad per player | done | host build: two fake controllers at once, input and rumble each, audio to player 1 only; board with one real controller; players 2-4 untested with real controllers |
| Pairing a controller over the board's Bluetooth (C6 via ESP-Hosted, NimBLE on the P4) | done | board (2026-11-16, after PR #24): finds the controller, reads its key, sends the hotspot's credentials; the controller joined and connected about 20 s later without the usual off and wake |
| Firmware updates from the web console (two app slots, rollback, CI-built image on Pages) | done | board (2026-11-16): wired flash of the new layout boots from ota_0; "Install latest" of the CI build (ESP-IDF 5.5.6, PR #21) installs over USB and the controller connects |

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
| #16 | TLS and audio buffers in PSRAM, faster flash | `MBEDTLS_EXTERNAL_MEM_ALLOC` (boots and reaches ready on the board); audio rings and queues (about 55 KB) in PSRAM via `port_alloc_bulk()`, Opus state and audio stacks stay internal; flash at QIO 80 MHz instead of DIO 40 MHz (80 MHz needs minimum chip rev v1.0; boots on the board), aimed at the headphone underruns while the mic runs. Board-tested 2026-11-16: boots, and the mic works with Assistant held. Running code from PSRAM (`SPIRAM_XIP_FROM_PSRAM`) was tried and dropped: it boot-loops a v1.3 P4 |
| #17 | Adjustable Opus bitrate | headphone stream bitrate 32-160 kbps (default 96, menuconfig `DONGLE_OPUS_BITRATE`), set from the web console or `set bitrate N`, saved in NVS, applied from the next 20 ms frame. Host-tested (48 kbps gives 121 B frames); board test pending |
| #18 | Rumble settings and test | rumble on/off and strength per motor (0-200%) saved in NVS, applied in `main/rumble.c`; console `set rumble*` and `rumble <l> <r> [ms]` test, live motor levels, a Rumble card on the page. Host-tested; board-tested 2026-11-16: the console's rumble test works |
| #19 | Battery and connection quality in the web console | battery gauge (`ControllerToDisplay` #10) with its age; keepalive STUN round trip per link; input series sequence gaps and unanswered keepalives over the last minute; Good/Fair/Poor verdict. Host-tested with the fake controller (`--battery`, `--input-gap`); board-tested 2026-11-16: the battery reading looks right (the connection figures not yet checked against a weak link) |
| #20 | Firmware updates from the web console | two app slots (`ota_0`, `ota_1`; `c6_fw` stays at 0x310000) and bootloader rollback; `ota begin <size> <sha256>` then the raw image over the console's serial port, 16 KB window, into a PSRAM stream buffer; SHA-256, ESP-IDF image and project-name checks before the switch; the new image is kept once USB is up for 10 s. CI builds the app and full-flash images and publishes them with the console on Pages. Needs one wired flash for the new layout. Board-tested 2026-11-16 (wired flash of the new layout, then Install latest once #21 landed) |
| #21 | Console updates: CI build that works on the board, safer trial | the first CI image (ESP-IDF 5.5.4) failed on the board in `mbedtls_ecp_gen_key` (-0x4d00, ECP random failed): no DTLS certificate, so no servers, and the controller joined the hotspot but got no answers; the same commit built with 5.5.6 works. CI now builds with 5.5.6 and the registry's components, as on the PC (git copies only if the registry fails). A new image whose servers don't start rolls back at once instead of being kept after 10 s. Board-tested 2026-11-16: Install latest from the console, the controller connects |
| #23 | Several controllers and pairing on the dongle | up to 4 players (`set players N`, after a restart): each controller gets a player number by its device ids or display token, with its own GMsg queue and links on UDP 40000+2n / 40001+2n, kept in NVS `players`; slot 0's state stays in internal RAM, the others' in PSRAM. Xbox 360 and HID modes present one gamepad per player (endpoints 0x81, 0x85, 0x86, 0x87; Xbox with several pads uses 1209:0001 and one MS OS 2.0 `XUSB10` function per pad). Audio, mic and mode combos are player 1's. Pairing (`main/pair.c`, console `pair`): NimBLE on the P4 over ESP-Hosted VHCI to the C6's radio, the same exchange as `stadiacast-provision` (crypto window 1a, RSA PKCS#1 v1.5 password, connect JSON) with the hotspot's name and password. Console Controllers card. Host-tested with two fake controllers (`STADIA_PLAYERS=2`, `--device-id`); on the board with one real controller (see #24) |
| #24 | Pairing fixes from the first board test | the first try connected to another Fast Pair advertiser (phones and earbuds advertise 0xFE2C too) and failed; the second read the controller's 4.4 KB crypto window and then rebooted the board: `tinfl_decompress_mem_to_mem` keeps its 10.5 KB decompressor on the stack, more than the 8 KB pairing task had. The decompressor and the RSA buffers are now on the heap (task stack 12 KB), and the scan lists every candidate for 3 s, tries Stadia-named then strongest first, and skips a device without the setup service. Board-tested 2026-11-16: found the controller, read its key, sent the credentials; the controller joined the hotspot and reached a game session about 20 s after pairing, with no off and wake |
| #26 | Lower Opus complexity while the mic runs | the mic's decoder and the headphones' encoder share the CPU: while push to talk is held or speech (not DTX) arrives, the encoder drops to the mic complexity (`set mic_complexity N`, default 4, only when below the normal one) and goes back 1 s after the mic goes quiet. On a PC build of the same fixed-point libopus, complexity 4 encodes in about half the time of 8 (5 in about 75%): 4 drops CELT's pitch pre-filter and the second MDCT on transients. The console's audio card shows the complexity in use. Opus was already fixed point (`FIXED_POINT`, `DISABLE_FLOAT_API`); `-O3` was about 10% faster on the PC but 50% more code, which on the P4 runs from flash through the cache, so it stays `-O2`. Board-tested 2026-11-16: switches to 4 on the press and back about 1 s after release; the underruns did not change (see #27, #28) |
| #27 | Fill in headphone packets the USB endpoint missed | board test of #26: 3 underruns per 10 s whatever the complexity (4, 8, 10) or the mic, so not the CPU. The USB endpoint got about 79,530 packets per 10 s instead of 80,000 (0.6% short, 497 Opus frames instead of 500): high-speed iso OUT is one 24-byte packet per 125 us microframe, re-armed in the interrupt, and a packet that comes before the re-arm is lost. The interrupt now reads the controller's (micro)frame number (`USB_DWC_HS.dsts_reg.soffn`); a step of more than one means packets were missed, and the current packet is repeated in their place (up to 8; 64 or more is a pause), so the stream keeps its length. The log's usb_audio line and the console's audio card show the missed packets. Complexity 10 stutters on the board (encode up to 15 ms per 20 ms frame): keep 8. Board-tested 2026-11-16: confirmed ~500 missed per 10 s (79,530 + 500 = 80,030), but in bursts too long to fill (see #28) |
| #28 | The web console's task list no longer stalls the USB headphones | the board test of #27 counted ~500 missed USB packets per 10 s but only ~80 filled in: the misses came in about ten bursts of ~50 packets (6 ms), one a second, which is the console's stats tick. `uxTaskGetSystemState()` holds the kernel's critical section (interrupts off) while it measures every task's free stack, and the USB interrupt could not run. CPU per core now comes from `ulTaskGetIdleRunTimeCounterForCore()` (a few reads), and the task list runs every 2 s only while the headphones are idle, or once on `tasks` (the Tasks card's Refresh button). This also explains why the underruns were about 0 in the PR #14 test (before the console) and 3 per 10 s since. Board-tested 2026-11-16 with the console open: 80,000 packets and 0 missed per 10 s, 0 underruns for 2 minutes with and without Assistant held; encode max at complexity 8 down from about 14 ms to about 9 ms |
| #30 | Restructure 1/4: portable core | the protocol and servers (`firmware/core/stadia`), libopus (`core/opus`) and the USB gamepad modes (`core/gamepad`, one `mode_<name>.c` each) moved out of the ESP32-P4 project; the OS layer is `firmware/platform/` (`stadia_port.h`, ESP-IDF and POSIX ports); the Linux build is `firmware/host` and runs in CI (gamepad report hashes, loopback against the fake controller). No behaviour change: the modes' reports are byte-identical, and the P4 image has the same RAM layout (+240 bytes of flash). Layout in [firmware/README.md](../../README.md). Board test pending |
| #31 | Restructure 2/4: the dongle's logic in the core | input mapping, the mode combos and BOOT button, rumble settings and test, players and the LED patterns moved from `main/` to `firmware/core/dongle`, behind `dongle_board.h` (key/value store, LED, button, USB mode), which the board implements in `components/dongle_board` (NVS, GPIO3, GPIO35, usb_pad). `stadia_port.h` gained a mutex and timers. New host test `dongle_test` runs the logic on a fake board (combos, the button, rumble scaling and test, players in the store). The P4 image: same code paths, 8 bytes less .bss and 64 less .data. Board test pending |

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

- Slight clipping on some loud songs. Likely fix: a few dB of headroom before the Opus encoder
  (the console's headphone gain).
- The microphone is untested on Windows.
- Players 2 to 4 are untested with real controllers (one real controller so far; pairing is
  tested with it).
- The Opus bitrate setting has not had a listening test.
- DualShock 4 and Switch Pro rumble, and Switch Pro mode on a real Switch, are untested.

## Next

1. Audio headroom for the clipping above, and fewer headphone underruns while the mic runs:
   the web console's gain and Opus complexity settings make both easy to try on the board.
2. Listen to a few bitrates (for example 64, 96 and 128 kbps) and pick the default.
3. Try the microphone on Windows.
4. A second real controller for players 2 to 4.
5. Test the Switch Pro mode on a Switch, and rumble in DS4 and Switch modes.
