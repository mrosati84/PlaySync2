#!/bin/sh
# Item 4: a hard recv() error must close the connection immediately, not linger
# until the 15 s liveness reap. recv() failure is injected with LD_PRELOAD.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CC=${CC:-cc}
if ! $CC -shared -fPIC -O2 -o tests/fail_recv.so tests/fail_recv.c -ldl 2>/dev/null; then
    echo "recv-error: SKIP (cannot build LD_PRELOAD shim)"
    exit 0
fi

exec python3 - <<'PY'
import os, socket, subprocess, sys, time

PORT = 18883
so = os.path.abspath("tests/fail_recv.so")
env = dict(os.environ, LD_PRELOAD=so, PS_FAIL_RECV_AFTER="1")
srv = subprocess.Popen(["./playsync2", "server", "--port", str(PORT),
                        "--max-members", "4", "--quiet"],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
fail = 0

def expect(cond, msg):
    global fail
    print(("ok: " if cond else "FAIL: ") + msg)
    if not cond:
        fail = 1

try:
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(3)
    s.sendall(b'{"t":"hello","v":1,"id":"a","name":"A","observer":false}\n')

    # Drain the handshake, then wait for the server to close promptly.
    closed = False
    deadline = time.time() + 3.0
    while time.time() < deadline:
        s.settimeout(max(0.1, deadline - time.time()))
        try:
            d = s.recv(4096)
        except socket.timeout:
            break
        if not d:
            closed = True
            break
    expect(closed, "hard recv error closes the connection immediately")
finally:
    srv.terminate()
    try:
        srv.wait(timeout=3)
    except subprocess.TimeoutExpired:
        srv.kill()

print("recv-error:", "FAIL" if fail else "ok")
sys.exit(fail)
PY
