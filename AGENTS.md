# Notes for AI coding agents

Read this before changing anything. It is the short version of what a person would have to
tell you; the long versions are linked. `CLAUDE.md` points here, so every tool reads the same
file.

## What this is

Firmware that turns a microcontroller board into a USB dongle for the Stadia Controller: the
controller joins the board's Wi-Fi hotspot, and the board is a USB gamepad (rumble, headset
audio, microphone) to the computer. One board so far: the DFRobot FireBeetle 2 ESP32-P4.
The Python package `stadiacast` (pinned in `pyproject.toml`) is only used by the tests.

## Where things are

```
firmware/core/          portable C, no ESP-IDF, no board: change behaviour here
  stadia/               the controller's side (HTTPS, signalling, DTLS-SRTP, sessions, audio, mic)
  gamepad/              USB gamepad modes, one mode_<name>.c each
  dongle/               input mapping, combos, rumble, players, LED, web console, OTA, pairing;
                        the board implements include/dongle_board.h (+ dongle_ble_t for pairing)
  opus/                 libopus 1.5.2 (git submodule), fixed point
firmware/platform/      stadia_port.h (OS layer) and its ESP-IDF and POSIX ports
firmware/host/          the core built for Linux, and its tests
firmware/esp32p4/       the FireBeetle board: ESP-IDF project, board code, web console page
  docs/progress.md      the project log: one row per merged PR
tests/fake_controller.py  a fake Stadia Controller for the loopback test
```

Rule of thumb: if the controller or the computer can see it, it belongs in `core/`. Only radio,
USB, storage, GPIO and the OS go in a board folder. The core must never include an ESP-IDF or
board header; it talks to the board only through `stadia_port.h` and `dongle_board.h`.

Long versions: [firmware/README.md](firmware/README.md) (layout, adding a gamepad mode,
adding a board), [firmware/esp32p4/README.md](firmware/esp32p4/README.md) (the board's user
guide), [firmware/esp32p4/docs/progress.md](firmware/esp32p4/docs/progress.md) (history and
"Things we learned the hard way").

## Build and test

The host tests need no hardware. Run all three after any change to `firmware/core` or
`firmware/platform`:

```sh
git submodule update --init
uv sync
cmake -S firmware/host -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls
cmake --build build-host -j
build-host/gamepad_test -v                # each gamepad mode's reports against known hashes
build-host/dongle_test                    # core/dongle on a fake board
firmware/host/run_loopback.sh build-host  # a whole controller session against the fake controller
```

Without ESP-IDF, any mbedTLS 3.6 source tree works for `MBEDTLS_DIR` (CI clones v3.6.4).

The board build (ESP-IDF 5.5.6):

```sh
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
```

CI: `.github/workflows/firmware-host.yml` runs the host tests; `console-pages.yml` builds the
board firmware on every PR and, on `main` and on each release, publishes the web console to GitHub
Pages with each board's latest release in `firmware/<board>/` (the console's "Install latest"
picks the dongle's board) and the build of `main` in `firmware/<board>/dev/`; `release.yml` turns a pushed `v*` tag into a GitHub release with the .bin
files and redeploys Pages. Both build through `firmware-build.yml`. Never push a tag
without the maintainer's OK.

## Rules learned the hard way

Each of these cost a board test to find. Don't undo them without a board test that says so.

- **Never call `uxTaskGetSystemState()` (or anything that disables interrupts for
  milliseconds) periodically while audio plays.** The web console's once-a-second task list
  did that and stalled the USB interrupt for about 6 ms, losing ~500 headphone packets per
  10 s: audible crackle. The task list (`board_tasks_json()`) runs only while the headphones
  are idle, or once on the `tasks` command. CPU per core comes from cheap idle-counter reads.
- **Opus state and the audio task stacks stay in internal RAM** (`port_alloc_fast()`).
  PSRAM (`port_alloc_bulk()`) is fine for buffers the CPU only copies through: TLS buffers,
  audio rings and queues. Opus in PSRAM made encoding slow and the audio scratchy.
- **Opus encoder complexity 8 or lower.** 10 stutters on the P4 (up to 15 ms to encode a
  20 ms frame). While the mic runs, the encoder drops to 4.
- **Libopus stays `-O2`.** `-O3` is 50% more code, which runs from flash through the cache.
- **Don't run code from PSRAM** (`SPIRAM_XIP_FROM_PSRAM`) on the FireBeetle P4: it boot-loops.
  Not worth retrying.
- **The FireBeetle's P4 is a v1.x chip (v1.3).** `sdkconfig.defaults` needs
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y`; builds for v3.x
  chips don't boot. ESP-IDF 5.5.2 or later is needed for that setting.
- **Build with ESP-IDF 5.5.6.** A 5.5.4 build failed on the board in `mbedtls_ecp_gen_key`
  (-0x4d00): no DTLS certificate, so the controller joined but got no answers.
- **Startup RAM is tight on v1.x P4s** (about 100 KB before the scheduler starts). Large
  buffers go on the heap, not in static arrays; a 1.4 KB static buffer boot-looped the board.
- **Send audio on a fixed 20 ms grid**, and keep the hotspot's DTIM period at 1.
- **High-speed iso OUT needs bInterval 1** on the P4's DWC2 controller.
- **Don't use the 045E:028E (Xbox 360) USB ID with headset audio**: Windows' xusb22 driver
  claims the whole device. Xbox mode with audio uses 1209:0001 with an MS OS 2.0 `XUSB10`
  descriptor.
- **Keep big buffers off small task stacks.** The pairing decompressor (10.5 KB) overflowed an
  8 KB stack and rebooted the board.

## Working on this repo

- Each PR adds a row to the Timeline table in `firmware/esp32p4/docs/progress.md` (what, and
  how it was tested: host, board, or "board test pending"), and updates Status and Known
  issues if they change. PRs up to #34 are in dinoyoshi/stadiacast; numbers restart in this
  repository.
- User-facing changes also update `firmware/esp32p4/README.md` (and the root README's tables
  for a new board or feature), and `firmware/esp32p4/docs/troubleshooting.md` if they add a new
  way for things to go wrong.
- A new gamepad mode: follow "Adding a gamepad mode" in `firmware/README.md`, including its
  hash in `firmware/host/gamepad_test.c`.
- A new board: follow [.claude/skills/port-board/SKILL.md](.claude/skills/port-board/SKILL.md)
  (plain Markdown; any agent can read it).
- Say plainly in the PR what was tested on hardware and what wasn't. Host tests passing does
  not mean the board works.
