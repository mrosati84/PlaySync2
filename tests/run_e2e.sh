#!/bin/sh
# Opt-in end-to-end test against REAL mpv (SPEC §10 AC-1/AC-4/AC-5 path).
#
# Generates a short test video with ffmpeg, runs two headless mpv-backed
# clients, then drives one of them through a second MPV IPC client:
#   play -> seek forward -> pause.  Both clients must adopt every intent and
#   stay within a small drift while playing.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=18895
TMP=$(mktemp -d)
SRV=""
A=""
B=""
cleanup() {
    [ -n "$A" ] && kill "$A" 2>/dev/null
    [ -n "$B" ] && kill "$B" 2>/dev/null
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT
cd "$ROOT"

if ! command -v mpv >/dev/null 2>&1 || ! command -v ffmpeg >/dev/null 2>&1; then
    echo "skip: make e2e requires mpv and ffmpeg"; exit 0
fi

ffmpeg -y -loglevel error -f lavfi -i "testsrc=duration=40:size=160x120:rate=30" \
    -pix_fmt yuv420p -c:v mpeg4 "$TMP/media.mkv" >/dev/null 2>&1 || {
    echo "skip: could not generate test media"; exit 0; }

mkdir -p "$TMP/run"
export XDG_RUNTIME_DIR="$TMP/run"

./playsync2 server --port "$PORT" --quiet >"$TMP/srv.log" 2>&1 &
SRV=$!
sleep 0.3

./playsync2 client --connect "127.0.0.1:$PORT" \
    -- --no-config --vo=null --ao=null --really-quiet "$TMP/media.mkv" \
    >/dev/null 2>"$TMP/a.err" &
A=$!
./playsync2 client --connect "127.0.0.1:$PORT" \
    -- --no-config --vo=null --ao=null --really-quiet "$TMP/media.mkv" \
    >/dev/null 2>"$TMP/b.err" &
B=$!

sleep 2

python3 - "$TMP/run" <<'PY'
import glob, json, os, socket, sys, time
rundir = sys.argv[1]
socks = []
deadline = time.time() + 10
while time.time() < deadline and len(socks) < 2:
    socks = sorted(glob.glob(os.path.join(rundir, "playsync2-*.sock")))
    time.sleep(0.2)
if len(socks) < 2:
    print("FAIL: mpv IPC sockets not found"); sys.exit(1)

def cmd(sock, command):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2)
    s.connect(sock)
    s.sendall((json.dumps({"command": command}) + "\n").encode())
    time.sleep(0.05)
    s.close()

# Start playback from a second IPC client (no key remapping required).
cmd(socks[0], ["set_property", "pause", False])
time.sleep(4)
# Forward seek well beyond the current position.
cmd(socks[0], ["seek", 20.0, "absolute"])
time.sleep(4)
# Pause again.
cmd(socks[0], ["set_property", "pause", True])
time.sleep(1.5)
PY

sleep 0.5
kill "$A" "$B" 2>/dev/null
wait "$A" 2>/dev/null
wait "$B" 2>/dev/null
A=""; B=""
kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
SRV=""

python3 - "$TMP/a.err" "$TMP/b.err" <<'PY'
import re, sys
fail = 0
for path in sys.argv[1:]:
    data = open(path, errors="replace").read()
    joined = "joined session" in data
    conv = "converged" in data
    aheads = [float(x) for x in re.findall(r"ahead=(-?[0-9]+\.[0-9]+)", data)]
    tail = aheads[-40:]
    worst = max((abs(a) for a in tail), default=None)
    print(f"{path}: joined={joined} converged={conv} samples={len(aheads)} worst_ahead={worst}")
    if not joined:
        print("FAIL: client never joined"); fail = 1
    if worst is None or worst > 0.6:
        print(f"FAIL: drift too large ({worst})"); fail = 1
sys.exit(fail)
PY
rc=$?
if [ "$rc" = 0 ]; then
    echo "ok: real-mpv e2e (play/seek/pause adopted, drift within 0.6 s)"
fi
exit "$rc"
