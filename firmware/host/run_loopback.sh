#!/bin/sh
# The host build of the core against the Python fake controller, end to end: boot requests,
# signalling, DTLS, READY, the game session (input and rumble), headset audio, the microphone
# echoed back, battery and link stats. Exits non-zero when any of it is missing.
#   firmware/host/run_loopback.sh [build dir]     (from anywhere; default build-host)
# Needs the Python environment (uv sync, see pyproject.toml) for tests/fake_controller.py.
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build=$(cd "${1:-$repo/build-host}" && pwd)
logs=$(mktemp -d)
https=${HTTPS_PORT:-8443}

STADIA_ALLOW_CLEARCUT=1 STADIA_START_GAME=1 STADIA_AUDIO=1 STADIA_MIC=1 STADIA_STATS=1 \
    "$build/stadia_host" 127.0.0.1 "$https" 40000 40001 > "$logs/host.log" 2>&1 &
host=$!
trap 'kill $host 2>/dev/null' EXIT  # the second run's too: \$host is read when it fires
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
check host.log 'gain \+12 dB \(built-in microphone, no headset\)' 'built-in mic gain (no headset)'
check host.log 'microphone [0-9.]+ s, 4[34][0-9] Hz, peak (2[0-8][0-9]{3}|29[01][0-9]{2}|2920[0-5])$' 'mic gain boosted, limited under -1 dBFS'
check host.log 'battery=76%' 'battery reading'
if [ $fail -ne 0 ]; then
    echo "--- fake controller (last 40 lines)"; tail -40 "$logs/fake.log"
    echo "--- host build (last 40 lines)"; tail -40 "$logs/host.log"
fi

# Two controllers at once: both headsets get the same sound, and both microphones (each echoing
# it) are mixed into the one stream.
STADIA_ALLOW_CLEARCUT=1 STADIA_START_GAME=1 STADIA_AUDIO=1 STADIA_MIC=1 STADIA_PLAYERS=2 STADIA_MIC_GAIN_BUILTIN=0 \
    "$build/stadia_host" 127.0.0.1 "$https" 40000 40001 > "$logs/host2.log" 2>&1 &
host=$!
sleep 1
for n in 1 2; do
    timeout 90 uv run --no-sync python tests/fake_controller.py --host 127.0.0.1 --https-port "$https" \
        --game 8 --mic-echo --device-id "fake-player-$n" > "$logs/fake2-$n.log" 2>&1 &
    eval "fake$n=\$!"
    sleep 2
done
wait $fake1; rc1=$?
wait $fake2; rc2=$?
sleep 1
kill $host 2>/dev/null
fail2=0
check2() { if grep -Eq "$2" "$logs/$1"; then echo "ok    $3"; else echo "FAIL  $3"; fail2=1; fi; }
[ $rc1 -eq 0 ] && [ $rc2 -eq 0 ] || { echo "FAIL  fake controllers exit codes $rc1 $rc2"; fail2=1; }
check2 fake2-1.log 'audio: [1-9][0-9]{2,} frames played .* 0 sequence gaps' 'two players: player 1 headset audio'
check2 fake2-2.log 'audio: [1-9][0-9]{2,} frames played .* 0 sequence gaps' 'two players: player 2 headset audio'
check2 host2.log "player 2's microphone stream" 'two players: player 2 microphone decoded'
check2 host2.log 'microphone [0-9.]+ s, 4[34][0-9] Hz' 'two players: microphones mixed (440 Hz)'
if [ $fail2 -ne 0 ]; then
    for f in fake2-1 fake2-2 host2; do echo "--- $f (last 30 lines)"; tail -30 "$logs/$f.log"; done
    fail=1
fi
[ -n "${LOOPBACK_KEEP:-}" ] && echo "logs kept in $logs" || rm -rf "$logs"
exit $fail
