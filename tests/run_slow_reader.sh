#!/bin/sh
# F01: a member that stops reading must not grow the relay's memory without
# bound; the relay drops it and keeps serving everyone else.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
exec python3 - <<'PY'
import json, socket, subprocess, sys, time

PORT = 18884
srv = subprocess.Popen(["./playsync2", "server", "--port", str(PORT), "--quiet"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
fail = 0

def expect(cond, msg):
    global fail
    print(("ok: " if cond else "FAIL: ") + msg)
    if not cond:
        fail = 1

def rss_kb():
    with open("/proc/%d/status" % srv.pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0

def join(ident, rcvbuf=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.connect(("127.0.0.1", PORT))
    s.settimeout(3)
    hello = {"t": "hello", "v": 1, "id": ident, "name": ident, "observer": False}
    s.sendall((json.dumps(hello) + "\n").encode())
    return s

def recv_line(s):
    b = b""
    while not b.endswith(b"\n"):
        d = s.recv(1)
        if not d:
            break
        b += d
    return b

try:
    slow = join("slow", rcvbuf=4096)
    recv_line(slow)  # welcome; never read again
    fast = join("fast")
    recv_line(fast)  # welcome
    time.sleep(0.2)
    base = rss_kb()

    hb = json.dumps({"t": "hb", "pos": 1.0, "state": "playing", "speed": 1.0,
                     "joining": False, "pad": "x" * 30000}, separators=(",", ":"))
    frame = (hb + "\n").encode()
    sent = 0
    while sent < 12 * 1024 * 1024:
        fast.sendall(frame)
        sent += len(frame)
    time.sleep(0.5)
    peak = rss_kb()
    print("relay RSS: %d kB before, %d kB after %d MB to a stalled reader"
          % (base, peak, sent >> 20))
    expect(peak - base < 6 * 1024, "relay memory stays bounded with a stalled reader")

    # The stalled member was dropped: draining its socket reaches EOF.
    slow.settimeout(5)
    eof = False
    try:
        while True:
            if not slow.recv(65536):
                eof = True
                break
    except OSError:
        pass
    expect(eof, "stalled reader is disconnected by the relay")

    # The remaining member is still served.
    fast.sendall(b'{"t":"ping","n":42}\n')
    got = None
    deadline = time.time() + 3
    buf = b""
    while time.time() < deadline and got is None:
        buf += fast.recv(65536)
        for line in buf.split(b"\n"):
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if m.get("t") == "pong":
                got = m
    expect(got is not None and got.get("n") == 42, "other members are still served")
finally:
    srv.terminate()
    try:
        srv.wait(timeout=3)
    except subprocess.TimeoutExpired:
        srv.kill()
sys.exit(fail)
PY
