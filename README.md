# PlaySync2

PlaySync2 keeps several [MPV](https://mpv.io/) players on the same content, in
sync, over the network. Every participant plays a **local copy** of the file:
there is no media transfer and no server-side authority. The server is a blind
message dispatcher and playback is agreed by a fully distributed
**lowest-position-wins** rule that runs identically in every client.

The frozen contract lives in [`SPEC.md`](SPEC.md) (the plan) and
[`PROTOCOL.md`](PROTOCOL.md) (wire protocol, min-wins algorithm, MPV contract).

## Build

```sh
make            # one binary: ./playsync2
make test       # unit + server-protocol + fake-MPV integration tests
make clean
```

Requires a C11 compiler and POSIX `poll(2)`/`clock_gettime`. `make e2e`
additionally drives **real** MPV and needs `mpv` plus `ffmpeg`; it is opt-in.

## Run

Start the relay on one machine:

```sh
playsync2 server --port 8765
```

Every viewer runs a client. The client spawns MPV itself; everything after `--`
is forwarded to MPV verbatim. The injected `--input-ipc-server`, `--keep-open`
and `--pause` options are appended last so they cannot be clobbered.

```sh
playsync2 client --connect 192.0.2.10:8765 -- /path/to/film.mkv --fullscreen
```

Everyone spawns paused; the first `resume` intent starts the group. New members
converge without disturbing anyone.

### Client options

| Flag | Meaning |
|---|---|
| `--connect HOST:PORT` | server to connect to (required) |
| `--name NAME` | member display name (default `$USER@$(hostname)`) |
| `--no-mpv` | observer only; no MPV is spawned, never eligible |
| `--show-drift` | show local drift as an MPV OSD overlay |
| `--allow-local-speed` | do not revert local speed changes (debug; breaks sync) |
| `--verbose` / `--quiet` | trace everything / suppress the live status line |

### Server options

| Flag | Meaning |
|---|---|
| `--bind ADDR` | listen address (default `0.0.0.0`) |
| `--port N` | listen port (default `8765`) |
| `--name NAME` | name used in the server's own logs |
| `--max-members N` | refuse beyond N (default 32, ceiling 64) |
| `--exit-when-empty` | exit when the last member leaves |
| `--verbose` / `--quiet` | log level |

See [`SPEC.md` §8](SPEC.md) for the full CLI reference.

## Design notes

- **Invariant A1** – one binary, two subcommands.
- **Invariant A2** – a single-threaded non-blocking `poll(2)` event loop on
  both sides; no threads and no blocking reads.
- **Invariant A3** – no cross-host timestamp arithmetic. Heartbeats carry only
  `(position, state, speed, joining)`; receivers extrapolate peers into their
  own monotonic clock domain using local elapsed time since receipt.
- The `server` never stores a position and never looks at playback payloads.
- `speed` is owned by the sync engine (it is the nudge mechanism); user speed
  changes are reverted.

The pure min-wins core is in `src/sync.c` and takes an injected `now`, which is
what the unit tests in `tests/test_sync.c` exercise directly.

## Layout

```
src/        main.c server.c client.c net.c proto.c json_mut.c mpv.c sync.c timebase.c
vendor/cJSON/   vendored cJSON
tests/      test_sync.c test_proto.c fake_mpv.c + integration scripts
Makefile    make / make test / make clean / make e2e
```

`tests/fake_mpv.c` implements the MPV JSON-IPC subset PlaySync2 uses plus a
simulated timeline, so the client can run headless in CI.

## Security

PlaySync2 is unauthenticated and unencrypted and never verifies that members
play the same file. Anyone who can reach the port can control everyone's
playback. It is a private-use tool: do not expose it to the internet.
