#!/bin/sh
# Server handshake / relay / error-path checks against a real server process.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
exec python3 - <<'PY'
import json, socket, subprocess, sys, time

PORT = 18881
srv = subprocess.Popen(["./playsync2", "server", "--port", str(PORT),
                        "--max-members", "2", "--quiet"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
fail = 0

def expect(cond, msg):
    global fail
    print(("ok: " if cond else "FAIL: ") + msg)
    if not cond:
        fail = 1

def conn():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.settimeout(3)
    return s

def send(s, obj):
    data = obj if isinstance(obj, str) else json.dumps(obj, separators=(",", ":"))
    s.sendall((data + "\n").encode())

_buf = {}

def read_one(s):
    b = _buf.get(s, b"")
    while b"\n" not in b:
        d = s.recv(4096)
        if not d:
            break
        b += d
    _buf[s] = b
    if b"\n" not in b:
        _buf[s] = b""
        return b.decode(errors="replace").strip()
    line, rest = b.split(b"\n", 1)
    _buf[s] = rest
    return line.decode(errors="replace").strip()

def recv_until(s, pred, limit=50):
    for _ in range(limit):
        line = read_one(s)
        if not line:
            return None
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if pred(m):
            return m
    return None

try:
    a = conn()
    send(a, {"t": "hello", "v": 1, "id": "a", "name": "A", "observer": False})
    expect((recv_until(a, lambda m: m.get("t") == "welcome") or {}).get("t") == "welcome",
           "hello -> welcome (A)")

    # F02: an id/name that would not fit the roster budget once escaped.
    x = conn()
    send(x, {"t": "hello", "v": 1, "id": "\u0001" * 63, "name": "\u0001" * 127,
             "observer": False})
    m = recv_until(x, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "too_large" and m.get("fatal") is True,
           "hello over the roster entry budget -> too_large")

    b = conn()
    send(b, {"t": "hello", "v": 1, "id": "b", "name": "B", "observer": False})
    expect((recv_until(b, lambda m: m.get("t") == "welcome") or {}).get("t") == "welcome",
           "hello -> welcome (B)")

    c = conn()
    send(c, {"t": "hello", "v": 1, "id": "c", "name": "C", "observer": False})
    m = recv_until(c, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "session_full" and m.get("fatal") is True,
           "session_full rejection")

    d = conn()
    send(d, {"t": "hello", "v": 1, "id": "a", "name": "D", "observer": False})
    m = recv_until(d, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "duplicate_id" and m.get("fatal") is True,
           "duplicate_id rejection")

    e = conn()
    send(e, {"t": "hello", "v": 99, "id": "e", "name": "E", "observer": False})
    m = recv_until(e, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "unsupported_version" and m.get("fatal") is True,
           "unsupported_version rejection")

    send(a, "this is not json")
    m = recv_until(a, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "bad_json" and not m.get("fatal"),
           "bad_json is non-fatal")

    send(a, {"t": "wat"})
    m = recv_until(a, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "unknown_type", "unknown_type is non-fatal")

    send(a, {"t": "ping", "n": 7})
    m = recv_until(a, lambda m: m.get("t") == "pong")
    expect(m is not None and m.get("n") == 7, "ping -> pong echoes n")

    send(a, {"t": "hb", "pos": 1.5, "state": "playing", "speed": 1.0, "joining": False})
    m = recv_until(b, lambda m: m.get("t") == "hb")
    expect(m is not None and m.get("from") == "a" and m.get("pos") == 1.5,
           "hb relayed with server-stamped from")

    send(a, {"t": "intent", "act": "pause", "pos": 2.0, "from": "evil"})
    m = recv_until(b, lambda m: m.get("t") == "intent")
    expect(m is not None and m.get("from") == "a" and m.get("act") == "pause",
           "intent relay discards client-supplied from")

    # F02: a legal frame that stamping/escaping grows past the cap is not
    # relayed; the sender gets a non-fatal too_large and stays connected.
    raw = '{"t":"hb","pos":1,"state":"playing","speed":1,"joining":false,"pad":"%s"}'
    grow = raw % ("x" * (65535 - len(raw % ""))) # exactly at the input cap
    send(a, grow)
    m = recv_until(a, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "too_large" and not m.get("fatal"),
           "message over the cap once relayed -> non-fatal too_large")
    send(a, {"t": "intent", "act": "resume", "pos": 3.0})
    m = recv_until(b, lambda m: m.get("t") in ("hb", "intent"))
    expect(m is not None and m.get("t") == "intent" and m.get("act") == "resume",
           "oversized relay dropped; recipient connected and served")

    big = '{"t":"hb","pad":"' + ("x" * 70000) + '"}'
    send(a, big)
    m = recv_until(a, lambda m: m.get("t") == "error")
    expect(m is not None and m["code"] == "too_large", "oversized message -> too_large")
finally:
    srv.terminate()
    try:
        srv.wait(timeout=3)
    except subprocess.TimeoutExpired:
        srv.kill()

print("protocol:", "FAIL" if fail else "ok")
sys.exit(fail)
PY
