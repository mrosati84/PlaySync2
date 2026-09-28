#!/bin/sh
# RLY-100: a commanded seek that targets the position MPV is already at must not
# leave a pending record that expires into "command not confirmed before deadline".
#
# A raw peer sends a redundant seek (pos 0, where the paused fake MPV already is)
# and then a real seek to prove the intent relay path is live. The client must
# not log the spurious warning.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
exec python3 - <<'PY'
import json, os, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.getcwd()
PORT = 18893
tmp = tempfile.mkdtemp()
rundir = os.path.join(tmp, "run")
os.makedirs(rundir)

env = dict(os.environ)
env["XDG_RUNTIME_DIR"] = rundir
env["PLAYSYNC2_MPV"] = os.path.join(ROOT, "tests", "fake_mpv")
env["FAKE_MPV_DURATION"] = "100"
env["FAKE_MPV_LOG"] = os.path.join(tmp, "mpv.log")

srv = subprocess.Popen(["./playsync2", "server", "--port", str(PORT), "--quiet"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
cli = None
s = None
fail = 0
_buf = b""


def expect(cond, msg):
    global fail
    print(("ok: " if cond else "FAIL: ") + msg)
    if not cond:
        fail = 1


def read_one(sock):
    global _buf
    while b"\n" not in _buf:
        d = sock.recv(4096)
        if not d:
            break
        _buf += d
    line, _, rest = _buf.partition(b"\n")
    _buf = rest
    return line


def last_pos(path):
    pos = None
    try:
        with open(path, errors="replace") as f:
            for line in f:
                for tok in line.split():
                    if tok.startswith("pos="):
                        pos = float(tok[4:])
    except OSError:
        pass
    return pos


try:
    cerr_path = os.path.join(tmp, "c.err")
    cerr = open(cerr_path, "wb")
    cli = subprocess.Popen(
        ["./playsync2", "client", "--connect", "127.0.0.1:%d" % PORT, "--",
         os.path.join(tmp, "media.mkv")],
        stdout=subprocess.DEVNULL, stderr=cerr, env=env)

    joined = False
    for _ in range(50):
        time.sleep(0.2)
        if b"joined session" in open(cerr_path, "rb").read():
            joined = True
            break
    expect(joined, "client joined the session")
    time.sleep(0.5)

    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(3)

    def send(obj):
        s.sendall((json.dumps(obj, separators=(",", ":")) + "\n").encode())

    send({"t": "hello", "v": 1, "id": "peer", "name": "Peer", "observer": False})
    welcome = False
    for _ in range(20):
        if b'"t":"welcome"' in read_one(s):
            welcome = True
            break
    expect(welcome, "peer joined as a member")

    # Target equals the client's current (paused) MPV position.
    send({"t": "intent", "act": "seek", "pos": 0.0})
    time.sleep(1.5)
    # A real seek proves the intent relay path is live, not just ignored.
    send({"t": "intent", "act": "seek", "pos": 5.5})
    time.sleep(2.0)

    data = open(cerr_path, "rb").read()
    if b"command not confirmed" in data:
        print("--- client stderr (warn lines) ---")
        for ln in data.decode(errors="replace").splitlines():
            if "warn" in ln:
                print(ln)
        print("--- end ---")
    expect(b"command not confirmed" not in data,
           "no spurious pending-command warning on a redundant seek")
    expect((last_pos(env["FAKE_MPV_LOG"]) or 0.0) > 4.0,
           "non-redundant seek was relayed and applied")
finally:
    if s is not None:
        s.close()
    if cli is not None:
        cli.terminate()
        try:
            cli.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cli.kill()
    srv.terminate()
    try:
        srv.wait(timeout=3)
    except subprocess.TimeoutExpired:
        srv.kill()
    shutil.rmtree(tmp, ignore_errors=True)

print("redundant seek:", "FAIL" if fail else "ok")
sys.exit(fail)
PY
