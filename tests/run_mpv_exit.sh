#!/bin/sh
# FR-1.4 / FR-1.5: the client propagates MPV's exit status and removes its socket.
# Also RLY-100: the bye reason is mpv_exit, and --verbose keeps the live status line.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=18891
TMP=$(mktemp -d)
SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
cd "$ROOT"

# Server runs at info level so the received bye reason is logged.
./playsync2 server --port "$PORT" >"$TMP/srv.log" 2>&1 &
SRV=$!
sleep 0.3

RUNDIR="${XDG_RUNTIME_DIR:-/tmp}"
before=$(find "$RUNDIR" -maxdepth 1 -name 'playsync2-*.sock' 2>/dev/null | wc -l)

FAKE_MPV_DURATION=100 FAKE_MPV_QUIT_AT=1.0 FAKE_MPV_EXIT_CODE=7 \
PLAYSYNC2_MPV="$ROOT/tests/fake_mpv" \
    timeout 8 ./playsync2 client --connect "127.0.0.1:$PORT" --verbose -- "$TMP/media.mkv" \
    >/dev/null 2>"$TMP/c.err"
rc=$?

after=$(find "$RUNDIR" -maxdepth 1 -name 'playsync2-*.sock' 2>/dev/null | wc -l)

fail=0
if [ "$rc" != "7" ]; then
    echo "FAIL: client exit status $rc, expected 7 (FR-1.5)"; fail=1
else
    echo "ok: mpv exit status propagated (7)"
fi
if [ "$after" != "$before" ]; then
    echo "FAIL: IPC socket not cleaned up (before=$before after=$after)"; fail=1
else
    echo "ok: IPC socket removed on exit (FR-1.4)"
fi
if grep -q '"reason":"mpv_exit"' "$TMP/c.err" 2>/dev/null && \
   grep -q 'sent bye (mpv_exit)' "$TMP/srv.log" 2>/dev/null; then
    echo "ok: bye reason is mpv_exit when MPV exits (RLY-100)"
else
    echo "FAIL: bye reason was not mpv_exit"; fail=1
    echo "  client: $(grep -o '"t":"bye"[^]]*' "$TMP/c.err" 2>/dev/null | tail -1)"
    echo "  server: $(grep 'sent bye' "$TMP/srv.log" 2>/dev/null | tail -1)"
fi
if grep -q 'state=' "$TMP/c.err" 2>/dev/null; then
    echo "ok: live status line kept under --verbose (FR-6.1)"
else
    echo "FAIL: --verbose suppressed the live status line"; fail=1
fi
exit "$fail"
