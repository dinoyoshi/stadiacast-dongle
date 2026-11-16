#!/bin/sh
# The host build of the core against the Python fake controller, end to end: boot requests,
# signalling, DTLS, READY, the game session (input and rumble), headset audio, the microphone
# echoed back, battery and link stats. Exits non-zero when any of it is missing.
#   firmware/host/run_loopback.sh [build dir]     (from anywhere; default build-host)
# Needs the repository's Python environment (uv sync) for tests/fake_controller.py.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build=$(cd "${1:-$repo/build-host}" && pwd)
logs=$(mktemp -d)
https=${HTTPS_PORT:-8443}

STADIA_ALLOW_CLEARCUT=1 STADIA_START_GAME=1 STADIA_AUDIO=1 STADIA_MIC=1 STADIA_STATS=1 \
    "$build/stadia_host" 127.0.0.1 "$https" 40000 40001 > "$logs/host.log" 2>&1 &
host=$!
trap 'kill $host 2>/dev/null' EXIT
sleep 1

cd "$repo" || exit 1
timeout 90 uv run --no-sync python tests/fake_controller.py --host 127.0.0.1 --https-port "$https" \
    --game 8 --mic-echo --battery 0.76 > "$logs/fake.log" 2>&1
rc=$?
sleep 1
kill $host 2>/dev/null

fail=0
check() { # file, pattern, what
    if grep -Eq "$2" "$logs/$1"; then echo "ok    $3"; else echo "FAIL  $3"; fail=1; fi
}
[ $rc -eq 0 ] || { echo "FAIL  fake controller exit code $rc"; fail=1; }
check fake.log 'GAME_STARTED' 'game session up (StartGame, DTLS)'
check fake.log 'motors left=[1-9][0-9]* right=[1-9]' 'rumble reached the motors'
check fake.log 'done: [0-9]+ motor writes, 0 watchdog stops, 0 framing errors' 'rumble held through the watchdog'
check fake.log 'audio: [1-9][0-9]{2,} frames played .* 0 sequence gaps' 'headset audio played without gaps'
check host.log 'microphone [0-9.]+ s, 4[34][0-9] Hz' 'microphone decoded (440 Hz echo)'
check host.log 'battery=76%' 'battery reading'
if [ $fail -ne 0 ]; then
    echo "--- fake controller (last 40 lines)"; tail -40 "$logs/fake.log"
    echo "--- host build (last 40 lines)"; tail -40 "$logs/host.log"
fi
rm -rf "$logs"
exit $fail
