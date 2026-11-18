# Contributing

Thanks for helping! The most useful things right now, roughly in order:

- **Test reports.** Anything marked ⚪ in the README's [Tested on](README.md#tested-on) table:
  the microphone on Windows, macOS, a Switch, rumble in DualShock 4 and Switch Pro modes, two or
  more real controllers. Open a
  [Tested on my setup](https://github.com/dinoyoshi/stadiacast-dongle/issues/new?template=tested_on_my_setup.yml)
  report, whether it worked or not.
- **Bug reports**, with the firmware version and the console's log
  ([bug report form](https://github.com/dinoyoshi/stadiacast-dongle/issues/new?template=bug_report.yml)).
- **A port to another board**: see [Adding a board](firmware/README.md#adding-a-board).
- Fixes and features in the firmware or the web console.

## Getting the code

```sh
git clone --recurse-submodules https://github.com/dinoyoshi/stadiacast-dongle.git
cd stadiacast-dongle
git submodule update --init      # if you cloned without --recurse-submodules (libopus)
```

[firmware/README.md](firmware/README.md) explains the layout. In short: anything the controller
or the computer can see belongs in the portable core (`firmware/core/`, plain C, no ESP-IDF);
only radio, USB, storage, GPIO and the OS go in a board folder such as `firmware/esp32p4/`.

## Run the host tests

They need no hardware and run in CI on every pull request. Run all three after any change under
`firmware/core` or `firmware/platform`. You need CMake, a C compiler, [uv](https://docs.astral.sh/uv/)
and an mbedTLS 3.6 source tree (ESP-IDF has one, or `git clone -b v3.6.4 --depth 1
https://github.com/Mbed-TLS/mbedtls.git`, as CI does).

```sh
uv sync --locked                 # Python environment for the fake controller
cmake -S firmware/host -B build-host -DMBEDTLS_DIR=$IDF_PATH/components/mbedtls/mbedtls
cmake --build build-host -j
build-host/gamepad_test -v                # each gamepad mode's reports against known hashes
build-host/dongle_test                    # the dongle's logic and web console on a fake board
firmware/host/run_loopback.sh build-host  # a whole controller session against the fake controller
```

A new or changed gamepad mode changes its report hash in `firmware/host/gamepad_test.c`: follow
[Adding a gamepad mode](firmware/README.md#adding-a-gamepad-mode).

## Build for the board

With [ESP-IDF 5.5.6](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32p4/get-started/index.html)
(5.5.4 builds boot but can't create the DTLS certificate):

```sh
cd firmware/esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash monitor     # the programming USB-C port, not the OTG one
```

Or install your `build/stadiacast_dongle.bin` from the web console's **Install a .bin file**.
CI builds the board firmware on every pull request too.

## Before you open a pull request

- **Read [AGENTS.md](AGENTS.md)'s "Rules learned the hard way".** Each one cost a board test to
  find (no periodic `uxTaskGetSystemState()` while audio plays, Opus state in internal RAM,
  complexity 8 or lower, big buffers on the heap, and so on). Don't undo one without a board
  test that says so.
- **Add a row to the Timeline** in
  [firmware/esp32p4/docs/progress.md](firmware/esp32p4/docs/progress.md): the PR number (link it,
  and write "(this repository)" as the rows there do), what changed, and how it was tested:
  `host`, `board`, or `board test pending`. Update Status and Known issues there if they change.
- **User-facing changes** also update the board's user guide
  ([firmware/esp32p4/README.md](firmware/esp32p4/README.md)), the
  [troubleshooting page](firmware/esp32p4/docs/troubleshooting.md) if it adds a new way to go
  wrong, and the root README's tables for a new board or feature.
- **Say plainly in the PR what was tested on hardware and what wasn't.** Host tests passing does
  not mean the board works.

## Releases

Versions are git tags `vMAJOR.MINOR.PATCH` on `main`. Pushing one runs the
[Release workflow](.github/workflows/release.yml), which builds the firmware at that commit and
publishes a GitHub release. Every file name carries the board it is for, so each board's build
can sit in the same release; for the FireBeetle 2 ESP32-P4 that is
`stadiacast-dongle-firebeetle2-esp32p4-full.bin` (first install),
`stadiacast-dongle-firebeetle2-esp32p4.bin` (console update) and
`firmware-firebeetle2-esp32p4.json`, plus one `SHA256SUMS` for all of them:

```sh
git tag -a v1.0.0 -m "v1.0.0"
git push origin v1.0.0
```

A tag with a hyphen (`v1.1.0-rc1`) makes a pre-release. Tag a commit that was tested on the board.
Publishing a full release (not a pre-release) also redeploys GitHub Pages, so the web console's
**Install latest** offers it to everyone. The build of `main` stays on Pages under
`firmware/<board>/dev/`, for testing before a release with **Install a .bin file**. Each board
has its own folder, and the console picks the one for the board the dongle reports.

## License

By contributing you agree that your work is released under the [MIT license](LICENSE), like the
rest of the project.
