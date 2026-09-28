#!/bin/sh
# FR-5.4 / AC-6: server loss does not interrupt playback; clients reconnect and
# re-converge when the server returns.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=18892
TMP=$(mktemp -d)
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
cd "$ROOT"

start_server() {
    ./playsync2 server --port "$PORT" --quiet >>"$TMP/srv.log" 2>&1 &
    SRV=$!
}

start_server
sleep 0.3

FAKE_MPV_DURATION=1000 FAKE_MPV_AUTOPLAY_AT=0.5 FAKE_MPV_LOG="$TMP/a.log" \
PLAYSYNC2_MPV="$ROOT/tests/fake_mpv" \
    ./playsync2 client --connect "127.0.0.1:$PORT" -- media.mkv >/dev/null 2>"$TMP/a.err" &
A=$!
FAKE_MPV_DURATION=1000 FAKE_MPV_LOG="$TMP/b.log" \
PLAYSYNC2_MPV="$ROOT/tests/fake_mpv" \
    ./playsync2 client --connect "127.0.0.1:$PORT" -- media.mkv >/dev/null 2>"$TMP/b.err" &
B=$!

sleep 3
kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
SRV=""
sleep 2
start_server
sleep 3

kill "$A" "$B" 2>/dev/null
wait "$A" 2>/dev/null
wait "$B" 2>/dev/null
kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
SRV=""

joins=$(grep -c 'joined session' "$TMP/a.err")
p1=$(awk -F'pos=' '/pos=/{split($2,f," ");p=f[1]} END{print p}' "$TMP/a.log")
p2=$(awk -F'pos=' '/pos=/{split($2,f," ");p=f[1]} END{print p}' "$TMP/b.log")

fail=0
if [ "$joins" -lt 2 ]; then
    echo "FAIL: client did not reconnect (joins=$joins)"; fail=1
else
    echo "ok: client reconnected ($joins joins)"
fi
awk -v p="$p1" 'BEGIN{ if (p > 6.0) exit 0; exit 1 }' \
    && echo "ok: playback continued through the outage (pos=$p1)" \
    || { echo "FAIL: playback stopped during outage (pos=$p1)"; fail=1; }
exit "$fail"
