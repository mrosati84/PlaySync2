#!/bin/sh
# Headless integration test driven by tests/fake_mpv.
#
# Two clients share one server. Client A autoplays, forward-seeks (50 s) and
# then pauses, all inside its fake MPV; client B only ever receives intents.
# Success: B follows A's seek and pause, and the two end within 0.6 s of each
# other, proving relay + intent adoption + min-wins convergence without real
# MPV or a display.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=18877
TMP=$(mktemp -d)
SRV=""

cleanup() {
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

cd "$ROOT"

./playsync2 server --port "$PORT" --quiet >"$TMP/srv.log" 2>&1 &
SRV=$!
sleep 0.3

FAKE_MPV_DURATION=100 \
FAKE_MPV_AUTOPLAY_AT=1.0 \
FAKE_MPV_SEEK_AT=3.0:50.0 \
FAKE_MPV_PAUSE_AT=5.0 \
FAKE_MPV_LOG="$TMP/a.log" \
PLAYSYNC2_MPV="$ROOT/tests/fake_mpv" \
    ./playsync2 client --connect "127.0.0.1:$PORT" --quiet -- "$TMP/media.mkv" \
    >/dev/null 2>"$TMP/a.err" &
A=$!
sleep 0.2

FAKE_MPV_DURATION=100 \
FAKE_MPV_LOG="$TMP/b.log" \
PLAYSYNC2_MPV="$ROOT/tests/fake_mpv" \
    ./playsync2 client --connect "127.0.0.1:$PORT" --quiet -- "$TMP/media.mkv" \
    >/dev/null 2>"$TMP/b.err" &
B=$!

sleep 7
kill "$A" "$B" 2>/dev/null
wait "$A" 2>/dev/null
wait "$B" 2>/dev/null
kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
SRV=""

last() { awk -F'pos=' '/pos=/{split($2,f," ");p=f[1]} END{print p}' "$1"; }
laststate() { awk '/state=/{for(i=1;i<=NF;i++)if($i ~ /^state=/){split($i,a,"=");s=a[2]}} END{print s}' "$1"; }

PA=$(last "$TMP/a.log")
PB=$(last "$TMP/b.log")
SA=$(laststate "$TMP/a.log")
SB=$(laststate "$TMP/b.log")

echo "client A: pos=$PA state=$SA"
echo "client B: pos=$PB state=$SB"

fail=0
case "$PA" in ''|*[!0-9.]*) echo "FAIL: no position from A"; exit 1;; esac
case "$PB" in ''|*[!0-9.]*) echo "FAIL: no position from B"; exit 1;; esac

# The forward seek to 50 s must have propagated to B (not reverted by min-wins).
awk -v p="$PB" 'BEGIN{ if (p < 45.0) { print "FAIL: B did not follow the forward seek (pos=" p ")"; exit 1 } }' || fail=1
# The pause must have propagated.
[ "$SB" = "paused" ] || { echo "FAIL: B did not adopt the pause (state=$SB)"; fail=1; }
# Both must have converged.
awk -v a="$PA" -v b="$PB" 'BEGIN{ d=a-b; if(d<0)d=-d; if (d > 0.6) { printf "FAIL: A and B diverged by %.3f s\n", d; exit 1 } }' || fail=1

# A redundant seek (target == current time-pos) must not leave a pending
# command that expires into a spurious warning (RLY-100).
if grep -q "command not confirmed" "$TMP/a.err" "$TMP/b.err" 2>/dev/null; then
    echo "FAIL: spurious 'command not confirmed' warning"
    fail=1
fi

if [ "$fail" = 0 ]; then
    echo "ok: fake-mpv integration (A followed by B, converged)"
fi
exit "$fail"
