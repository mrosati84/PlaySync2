# PlaySync2 — Specification

**Version:** 0.1 (spec frozen for implementation)
**Protocol version:** 1
**Status:** complete — every open question answered, see the [decision log](#13-decision-log)
**Companion document:** [`PROTOCOL.md`](PROTOCOL.md) — wire protocol, min-wins algorithm, MPV contract, verified MPV behaviours

---

## 1. Summary

PlaySync2 is a command-line utility written in C that keeps several [MPV](https://mpv.io/)
players on the same content, in sync, over the network. Every participant plays a **local
copy** of the file. There is no media transfer and no server-side authority: the server is
a **blind message dispatcher**, and the playback position is agreed by a fully
**distributed "lowest position wins"** rule that runs identically in every client.

- Linux and macOS. Windows is out of scope.
- The `client` role **launches and owns the MPV process**; users never start MPV themselves.
- The `server` role never touches video, never holds a position, and never looks at playback
  payloads.

---

## 2. Glossary

| Term | Meaning |
|---|---|
| **Server** | Headless process. Relays messages between members. Holds no playback state. |
| **Client** | Process that spawns MPV and participates in the session. One per participant. |
| **Member** | A client past the handshake. |
| **Heartbeat** (`hb`) | A member's periodic announcement of its position and state. The only source of position information in the system. |
| **Group minimum** | The lowest *extrapolated* position among all **eligible** peers. Undefined when no peer is eligible. |
| **Group position** | Where the group actually sits: the group minimum while it is playing, or the lowest position of the paused/EOF peers when it is not (PROTOCOL §4.3). What a joining member converges onto. |
| **Eligible** | A member whose position is meaningful enough to count toward the group minimum (see §5.3). |
| **Joining** | A member that has not yet converged with the group; its position is excluded from the minimum until it does. |
| **Ahead** | `local_position − group_minimum`; positive means this member is ahead. Always the only quantity ever corrected. |
| **Nudge** | Correction by temporarily lowering playback `speed`. |
| **Hard sync** | Correction by seeking backward onto the group minimum. |
| **Intent** | A user action that is broadcast to the group: `pause`, `resume`, `seek`. |

---

## 3. Architecture

```
┌─────────────┐                    ┌─────────────┐
│   mpv (A)   │                    │   mpv (B)   │
│ UNIX socket │                    │ UNIX socket │
└──────┬──────┘                    └──────┬──────┘
       │ mpv JSON IPC (spawned+owned by the client)
┌──────▼──────┐                    ┌──────▼──────┐
│  playsync2  │                    │  playsync2  │
│   client A  │                    │   client B  │
│ min-wins ↑  │                    │ min-wins ↑  │
└──────┬──────┘                    └──────┬──────┘
       │        TCP + newline-delimited JSON (star)
       └───────────────┬──────────────────┘
               ┌───────▼────────┐
               │ playsync2      │  blind relay:
               │    server      │  rebroadcast to all other members
               └────────────────┘
```

### 3.1 What the server is

- A TCP acceptor with a roster and a rebroadcast loop.
- The only per-member state it holds: socket, `id`, display `name`, the `observer` flag
  (PROTOCOL §3, rule 2), and a last-traffic timestamp for liveness. Nothing else.
- It stamps the sender identity (`from`) on every relayed message and nothing more. It does
  **not** read, interpret, validate, or store playback data.

### 3.2 What the server is not

- Not a player (it never runs MPV).
- Not a clock. **There is no server time base.** The server's monotonic clock is used only to
  time out dead connections.
- Not a source of truth. Two clients that both crashed the server would continue in sync.
- Not a file validator. It never sees file names, durations, or sizes.

### 3.3 Invariants

- **A1** Client and server are the same binary with different subcommands.
- **A2** All I/O is a single-threaded non-blocking event loop (`poll(2)`), client and server.
  No threads, no blocking reads.
- **A3** No cross-host timestamp arithmetic exists anywhere in the implementation. Positions
  are compared only after each receiver extrapolates them into *its own* clock domain using
  local elapsed time (§5.1). This is what makes clock synchronization unnecessary.
- **A4** Every message is a self-contained assertion, so TCP stream semantics are enough and
  no reordering sequence numbers are needed.
- **A5** The server relays each heartbeat to every other member, so its traffic is
  O(members² × heartbeat rate) — quadratic, but tiny in absolute terms at the documented cap
  (§14) — and always independent of media size.

---

## 4. Session model

- **One server, one session.** No rooms (original scope: private use, few users).
- **No authentication, no encryption** (original scope). See §11.
- Members are expected to be playing the same content. **PlaySync2 does not verify this in
  any way** and will happily try to sync two different films; see §11.
- **Start:** every client spawns MPV **paused at its own starting position** (normally
  frame 0). The first `resume` intent from any member starts the group. Members still loading
  catch up and join afterwards.
- **End:** MPV runs with `--keep-open=yes`, so finishing the file does not kill the process.
  The first member to hit EOF broadcasts a pause, and the group stops together (§5.8). The
  session is over when its members exit; the server returns to a waiting state.

---

## 5. Synchronization model — distributed "lowest position wins"

This is the core of the program and the direct answer to the open question in SPEC v0.1
("who owns the authoritative clock?"). The answer is: **no one.** There is no clock to own
because no timestamps cross the network. Each client compares positions expressed in *its
own* monotonic domain and converges downward onto the lowest one it can see.

### 5.1 No clock synchronization

Each member periodically announces `(position, state, speed)` in a heartbeat. A receiver
must compare that position with its own *now*. It does so using only its local receipt time:

```
Δ            = local_monotonic_now − local_monotonic_at_receipt
peer_now     ≈ peer_position + peer_speed × Δ
```

No clock offset, no shared epoch, and no ping/pong *for time*. The error in `peer_now` is
bounded by the peer→receiver one-way latency, which is never measured because it is never
needed:

- The estimate is biased **low**: a peer is always a little further along than the receiver
  believes. A member therefore believes it is further *ahead* than it is, so the rule
  over-corrects marginally rather than under-correcting. Over-correction keeps the group
  tight; under-correction would let it drift apart silently.
- Because every receiver under-estimates every peer by roughly the same amount — both legs of
  the star relay are nearly symmetric — the bias is largely *common*: it cancels out of the
  relative positions that actually matter, and the group moves together instead of splitting.
- The residual is absorbed by a deadband sized from the measured link (§5.5). The deadband
  must exceed the one-way latency, or a member in perfect sync would read itself as ahead by
  that latency and seek backwards forever, chasing a phantom offset that does not exist —
  that is the *only* reason an RTT is measured at all (PROTOCOL §2.5).

### 5.2 Heartbeats

- Emitted at **2 Hz** (every 500 ms) in all playback states; a paused or loading member still
  announces itself, it is just not eligible.
- Payload: local position, playback state, current speed, and the `joining` flag.
- Relayed by the server to all other members. Since a member's heartbeats reach a given peer
  in order (same TCP connection, same relay path), a stale heartbeat can never overwrite a
  newer one.

### 5.3 Eligibility — whose position counts

A peer's position counts toward the group minimum only if **all** of the following hold:

1. Its last heartbeat reports `state == playing`.
2. It is **not** seeking, buffering (`paused-for-cache`), or at EOF.
3. It is **not** `joining` (§5.7).
4. Its last heartbeat is at most 5 s old (PROTOCOL §4.2). A silent member's last known
   position is never trusted indefinitely.

Additionally, a member excludes *itself* from its own minimum computation — the minimum is
"the lowest position anyone else is at".

This is the entire protection against pathological members, and it is deliberately thin (see
§11). It excludes members whose reported position is *meaningless* (loading, joining,
buffering, seeking) — it does **not** protect against a member that is genuinely, slowly,
behind while playing. That member becomes the group minimum and the group keeps coming back
to it. This is an accepted consequence of choosing strict min-wins with no outlier
protection.

Members that are paused or parked at EOF are likewise excluded from the minimum (nothing is
advancing), but they still supply the position the group is sitting at, which is what lets a
joining member land on a paused group (§5.7).

### 5.4 The group minimum, and what may be done about it

```
group_min = min over eligible peers of (peer_position + peer_speed × Δ)
ahead     = local_position − group_min
```

- If no peer is eligible, `group_min` is undefined and **no correction ever happens**. A
  member alone in a session, or a member whose peers are all paused, simply plays.
- The rule is **one-sided**: only a member that is *ahead* is ever corrected. A member that
  is *behind* is not corrected, because it *is* the minimum and everyone else comes back to
  it. Nobody is ever seeked forward as part of drift correction, so the group never chases
  the fastest player.
- The exception is an explicit seek intent, which overrides the rule entirely (§5.6).

### 5.5 Correction policy

```
deadband = min( max(150 ms, rtt_self + 50 ms), 1000 ms )
rtt_self = EWMA of the last ~10 RTT samples to the server, α = 0.25 (PROTOCOL §2.5)
```

`rtt_self` stands in for the unmeasured one-way latency. A round trip is about twice a
one-way leg, so `rtt_self + 50 ms` is roughly twice the typical one-way error plus a floor —
deliberately generous, and a generous deadband only makes correction *less* eager. A jitter
spike can exceed it transiently; the result is a small over-correction, which the hysteresis
and the 5 s / 10 s rate limits bound.

The EWMA is chosen over a minimum or a maximum on purpose: a minimum would be undersized by a
single lucky sample and provoke over-correction, while a maximum would stay wide on a jittery
link and leave real drift uncorrected for too long.

Two honest caveats:

- **It is a heuristic, not a bound.** The true residual is the whole *peer→receiver* one-way
  latency — the peer's leg to the server plus this client's leg back from it — while the
  formula sees only this client's own link, and only its *typical* value. This client's
  inbound leg cancels between the two, so what decides is the difference between its outbound
  leg and the peers': when the peers sit much further from the server than this client does,
  that difference can outgrow the deadband, and the result is the slight over-correction
  described above rather than divergence — the tolerable failure.
- **It saturates at 1 s.** Beyond that the deadband stops growing and eventually equals the
  hard-sync threshold, so the nudge band vanishes and a link that slow gets hard seeks
  instead of speed nudges. That is the practical ceiling on link quality for which the
  design is sound.

| Status | Condition | Action |
|---|---|---|
| In sync | `ahead ≤ deadband` | nothing (unless a nudge is active — see *Exit nudge*) |
| Ahead, nudge | `deadband < ahead < 1 s` | `set_property speed (1 − clamp(ahead / 10 s, 0.01, 0.05))`, i.e. at most 5 % slower |
| Ahead, hard sync | `ahead ≥ 1 s` | `seek group_min absolute`, rate-limited to one per 10 s per member |
| Exit nudge | `ahead ≤ deadband / 2` | restore `speed` to `1.0` |
| Not playing | any non-playing `state` | restore `speed` to `1.0`, then nothing else |

Properties:

- **Hysteresis** (enter at `deadband`, exit at `deadband / 2`) prevents limit-cycling.
- **A nudge is always undone.** Leaving the playing state — for any reason — restores
  `speed` to 1.0 (PROTOCOL §4.4), so a paused, buffering or EOF member is never left
  permanently slowed by a correction that was interrupted mid-flight.
- **A nudge sheds at most 5 %**, so a 1 s excess takes up to ~20 s to absorb — slow,
  deliberate, and inaudible with MPV's default `audio-pitch-correction`.
- **Corrections are suppressed** whenever the local player is not in a clean playing state:
  paused, seeking, buffering, EOF, or not yet loaded. A member that is still catching up uses
  the catch-up rule (§5.7) *instead of* the correction rule.
- Corrections are **never** announced on the network. A nudge or hard sync is a local
  adjustment, not an intent, and must not be echoed or it would ping-pong.
- Hard syncs are rate-limited per member, so a pathological minimum cannot cause an unbounded
  seek storm — the group still rewinds onto it, but at most once every 10 s. The divergence
  that accumulates between those seeks is the real cost, and §11.1 quantifies it.

### 5.6 Explicit intents, and their precedence over min-wins

SPEC v0.1 requires that "any connected user can seek the video, and connected users receive
the seek event". Taken literally, that is incompatible with pure min-wins: if member A seeks
forward to 10:00 while B is at 5:00, then B is the minimum and min-wins would drag A straight
back to 5:00. Forward seeking would be impossible.

**Resolution: an explicit intent is authoritative and re-bases the group.**

- A `seek`, `pause`, or `resume` intent is transmitted verbatim to every member, including
  forward seeks, and every member adopts it regardless of the group minimum.
- After adoption, all members are at (or converging on) the same position, so the minimum
  moves with them and min-wins has nothing to correct. Min-wins governs **implicit drift
  only** — the deviation that accumulates on its own, which is exactly the problem SPEC v0.1
  created it for.
- The originating member does not re-apply its own intent on receipt (echo suppression,
  PROTOCOL §5.6); it already performed the action locally and supplies the resulting
  position, so it does not stutter.

**Pause snaps the group to the minimum.** A pause intent carries a concrete position, chosen
by the pauser as the minimum over itself and its eligible peers at that moment. Every member,
including the pauser, seeks *exactly* to that position — an intent is authoritative, so no
deadband applies to it — and pauses there. This is what makes a pause land everyone on the
*same frame* rather than freezing them up to a deadband apart. A `resume` carries the same
position and is applied the same way, so everyone plays from that same frame.

**Only `pause`, `resume` and `seek` are intents.** Volume, subtitles, and every other MPV
setting stay strictly personal and are never observed or transmitted.

### 5.7 Joining and catching up

A joining member's position carries no information about the session (it is wherever its MPV
happened to start), so it must not be allowed to rewind the group — and it must not be
allowed to act as a pause or seek *origin* either. One state covers both:

1. On start, a client is `joining`. It announces itself with `joining: true`, so peers see it,
   log it, and exclude it from every position rule.
2. **While joining, the member adopts the group's position and play state and sends no
   intents at all.** Any local play, pause or seek it performs during catch-up is suppressed
   and overridden; intents *received* from the group are applied as usual. This closes the
   obvious hole: a member that spawned paused at 0 and pressed play at 0 would otherwise
   broadcast `resume at 0` and yank the whole group back to the start of the file.
3. It converges onto the group. If any peer is playing, it seeks onto the lowest playing
   position and starts playing. If instead the group is paused (or parked at EOF), it seeks
   onto that position and stays paused. If neither can be determined, there is nothing to
   converge onto and the flag clears immediately.
4. Once it has been within the deadband of that position continuously for 1 s, it clears the
   flag and becomes eligible. From then on it can be the minimum, can drag the group, and its
   intents go live.
5. A member with no peers at all clears the flag immediately.

Existing members therefore never move because someone joined, which satisfies the original
requirement that new members must not disturb the session.

### 5.8 EOF

MPV is launched with `--keep-open=yes`, so it holds the last frame and sets `pause` itself
instead of exiting (verified — see PROTOCOL Appendix A). PlaySync2 must **not** mistake that
self-inflicted pause for a user pause.

- When a client observes `eof-reached`, it broadcasts an ordinary pause intent.
- It is ineligible at that moment (EOF excludes it from the minimum), so it applies the
  ordinary pause rule of §5.6: the position is the minimum over the pauser and its eligible
  peers, which here means the still-playing peers, all of them within a deadband behind the
  end. Every member — the finished one included — lands on that position, so the group stops
  together, on the same frame, at the end of the file.
- If the finished member is alone, the rule degenerates to its own position, exactly as it
  does for any pause with no eligible peers.
- No other special-casing exists; EOF reuses the pause path.

### 5.9 Speed is owned by the sync engine

`speed` is **not** a synced user setting. It is the nudge mechanism, and nothing else:

- A nudge sets `speed` to a value below 1.0; the client restores 1.0 when converged.
- A local `speed` change made inside MPV is an unexpected mutation of an engine-owned
  property: the client **reverts it** and logs it. Users cannot change playback speed while
  participating in a session. This is deliberate: a member holding a private 0.95× would fall
  permanently behind, become the group minimum, and (with no outlier protection, §5.3) drag
  everyone back forever on every correction cycle.

### 5.10 Worked examples

**Two members, one drifts ahead.**
A is at 100.02 s, B at 100.00 s, `deadband` 150 ms. B's minimum is A's 100.02 s; B is
`ahead` by −20 ms, i.e. not ahead at all. B does nothing (B is the minimum, it is never
pulled forward). A's minimum is B's 100.00 s; A is 20 ms ahead, inside the deadband, so A
does nothing too. Nothing happens — correct.

**One member is a full second ahead.**
A is at 101.00 s, B at 100.00 s. A computes `ahead = 1.00 s`, hits the hard-sync threshold,
and seeks to 100.00 s. B is the minimum and never moves.

**A member seeks forward.**
B presses the right-arrow key and lands at 130 s. B's client classifies the jump as a seek
intent, sends `intent{act:seek, pos:130.0}` and adopts it locally. A receives it, seeks
exactly to 130.0 s. Neither member consults the group minimum; the intent overrode it.
Had min-wins taken precedence, A (at 100 s) would have been the minimum and B would have
been yanked back.

**A third member joins mid-film.**
A and B are at ~130 s. C starts, at 0 s, `joining: true`. A and B ignore C's positions and
keep playing. C converges: it sees A and B playing at ~130 s, seeks there, adopts their play
state (so it starts playing too), and after a second inside the deadband it clears the flag.
Nobody else moved.

**A member's disk stalls.**
C's storage throttles; it keeps reporting `playing` but advances at half rate. C is eligible,
so C becomes the group minimum. A and B are corrected down onto C, repeatedly, at C's own
rate. This is the documented, accepted consequence of §5.3 and §11.

---

## 6. Functional requirements

`(v0.1)` marks the first milestone.

### FR-1 Roles and launch
- **FR-1.1** One binary, two subcommands: `server` and `client`.
- **FR-1.2** `server` listens on TCP (default `0.0.0.0:8765`), accepts up to
  `--max-members` members (default 32, hard ceiling 64 — PROTOCOL §1), and relays. It exits
  only on signal or `--exit-when-empty`.
- **FR-1.3** `client` connects to `host:port` and **spawns MPV itself**, injecting
  `--input-ipc-server=<socket>`, `--keep-open=yes` and `--pause=yes`, forwarding every
  argument after `--` verbatim.
- **FR-1.4** The MPV IPC socket is created as `$XDG_RUNTIME_DIR/playsync2-<pid>.sock`, falling
  back to `/tmp/playsync2-<pid>.sock`. An existing file at that path is unlinked first, and the
  socket is removed on every exit path.
- **FR-1.5** `client` exits when MPV exits, propagating MPV's exit status; a crash exits
  non-zero.
- **FR-1.6** `--no-mpv` runs a client with the network side only: an observer member that
  never launches MPV, never sends intents, and never becomes eligible.
- **FR-1.7** Non-zero exit with a one-line reason on stderr for fatal errors; `--help` and
  `--version` exit 0.

### FR-2 Play/Pause
- **FR-2.1** Any member may pause or resume, from inside MPV or from another IPC client. The
  action is detected, broadcast as an intent, and adopted by every member, including the
  originator (which does not re-apply it).
- **FR-2.2** A pause carries a position: the minimum over the pauser and its eligible peers.
  Every member seeks onto it and pauses there, so the group shares one frame.
- **FR-2.3** No key remapping is required: the user keeps their own `input.conf` and any MPV
  keybinding, OSC action, or script that changes `pause` or `time-pos` is detected
  (PROTOCOL §5.5).
- **FR-2.4** Resuming plays from the paused position for every member.
- **FR-2.5** A pause caused by `--keep-open` at EOF is not a user pause (§5.8).

### FR-3 Seek
- **FR-3.1** Any member may seek, forward or backward; the intent is adopted verbatim by
  every member and overrides the group minimum (§5.6).
- **FR-3.2** The originating member adopts the position it actually reached, so
  keyframe-quantised or relative MPV seeks relay the *achieved* position, not the requested
  one.
- **FR-3.3** A seek intent is not rate-limited and not delayed by an in-flight correction.

### FR-4 Drift correction
- **FR-4.1** While playing, each member computes `ahead` against the group minimum at 4 Hz
  and corrects it per §5.5.
- **FR-4.2** Correction is one-sided: members are never seeked forward, and no correction is
  ever transmitted to peers.
- **FR-4.3** Correction is suppressed while paused, seeking, buffering, at EOF, or still
  loading; a member that has not yet converged uses the catch-up rule instead (§5.7).
- **FR-4.4** Rate limits: at most one nudge *entry* per 5 s (restoring `speed` to `1.0` is
  never rate-limited, since it is always safe); at most one hard seek per 10 s.
- **FR-4.5** The client reverts any local `speed` change (§5.9).

### FR-5 Membership
- **FR-5.1** A member exists from `hello`/`welcome` until its connection drops or it sends
  `bye`.
- **FR-5.2** Join order does not matter; a joiner never moves an existing member (§5.7).
- **FR-5.3** A dropped connection removes the member immediately and the roster is
  rebroadcast to everyone.
- **FR-5.4** A client whose connection drops retries with exponential backoff (1 s → 15 s),
  keeping its local playback running, and returns as a **new member** that must re-converge.
  The server keeps no grace state for it.
- **FR-5.5** When the last member leaves, the server keeps listening with an empty roster.

### FR-6 Observability
- **FR-6.1** The client prints a live one-line status (state, position, group position, ahead,
  rtt, members) to stderr, refreshed at 4 Hz; suppressed by `--quiet`.
- **FR-6.2** `--verbose` traces every protocol message and every MPV command/event, with the
  direction, to stderr.
- **FR-6.3** The server logs connections, disconnections, and relay counters.
- **FR-6.4** `--show-drift` forwards a one-line `show-text` overlay to MPV (off by default).
- **FR-6.5** Log lines are single-line and greppable; no color when stderr is not a TTY.

---

## 7. MPV integration

The client **spawns** MPV and therefore owns its command line, its IPC socket, and its exit
status. It never requires the user to edit MPV configuration. Full detail, including the
observer table, the intent-classification rules and echo suppression, is in
[`PROTOCOL.md` §5](PROTOCOL.md).

```
playsync2 client --connect 192.0.2.10:8765 -- film.mkv --fullscreen
                 └────────────────────────┘   └───────────────────┘
                   our args                    forwarded verbatim to mpv

mpv argv actually used (ours appended last, so they cannot be clobbered):
   mpv film.mkv --fullscreen
       --input-ipc-server=$XDG_RUNTIME_DIR/playsync2-1234.sock
       --keep-open=yes --pause=yes
```

Verified MPV behaviours this design depends on (see PROTOCOL Appendix A for the evidence):

- `observe_property` fires `property-change` immediately on registration, and omits `data`
  entirely when the property is unavailable.
- `time-pos` property-change fires **once per decoded frame**, i.e. at the media frame rate —
  the drift loop must throttle rather than be driven by it.
- Multiple IPC clients may connect simultaneously and all receive all events.
- Closing a connection unregisters only that connection's observers; others are unaffected.
- `set_property time-pos` is a valid seek and works while paused.
- `seek <t> absolute` uses MPV's `exact` flag by default.
- With `--keep-open=yes`, EOF sets `pause` to true and holds the last frame.
- `speed` is settable, and `--audio-pitch-correction` (default on) keeps pitch via scaletempo2.
- The socket is not serviced while a command executes, so unrelated events can interleave
  before a reply — replies must be matched by `request_id`, never by position in the stream.

---

## 8. Command-line interface

```
playsync2 server [options]
    --bind ADDR          listen address            (default 0.0.0.0)
    --port N             listen port               (default 8765)
    --name NAME          server name for its logs  (default hostname)
    --max-members N      refuse connections beyond N (default 32, ceiling 64)
    --exit-when-empty    exit when the last member leaves
    --verbose | --quiet

playsync2 client --connect HOST:PORT [options] [-- mpv args...]
    --name NAME          member display name       (default $USER@$(hostname))
    --no-mpv             observer only; no MPV is spawned
    --show-drift         show local drift as an MPV OSD overlay
    --allow-local-speed  do not revert local speed changes (debug; breaks sync)
    --verbose | --quiet

playsync2 --version | --help
```

- Configuration is flags and environment only. No config file (§12).
- `server --name` never reaches a client: `welcome` carries an opaque `session` id instead
  (PROTOCOL §2.2), and Q22 keeps the wire free of anything but member names. The flag only
  labels the server's own log lines.
- Everything after `--` is forwarded to MPV untouched. PlaySync2 does not parse it, and does
  not need to know which argument is the media file.
- `--allow-local-speed` exists only for debugging the rewind-loop behaviour (§11) and is
  otherwise not a supported way to use the program; it prints a warning that it breaks
  synchronization.

---

## 9. Failure handling

| Situation | Required behaviour |
|---|---|
| Server unreachable at start | Client retries with exponential backoff 1 s → 15 s and prints the next attempt time. MPV keeps running locally. |
| Server disappears mid-playback | Client keeps playing undisturbed. With no peer heartbeats the group minimum is undefined, so it performs no correction until the server returns; it marks the session `partitioned` — the `BACKOFF` state of PROTOCOL §6, a purely local status, never a wire state — and retries. Playback is never interrupted. |
| Server restarts | Clients reconnect and resume from the same local positions; members re-converge within a few heartbeats. |
| MPV exits (user quits, or EOF without `--keep-open`) | Client exits with MPV's status (FR-1.5). |
| MPV IPC socket breaks while MPV runs | The client can no longer control or observe MPV, so it kills MPV and exits non-zero with a one-line reason. There is no re-attach; a half-attached client is worse than no client. |
| MPV rejects a command | Logged at `warn`, never fatal; the next 4 Hz tick re-issues it if it is still needed. |
| A peer stops sending heartbeats but the socket is alive | Treated as ineligible once its last heartbeat is older than 5 s (a stale position is never trusted). |
| Malformed message (not JSON, or an unknown `t`) | Answered with `error{bad_json}` / `error{unknown_type}`; **not** fatal, never disconnects (PROTOCOL §3). |
| Oversized message (> 64 KiB) | Connection closed with `error{too_large}`; the server logs the peer. |
| Unsupported protocol version | Rejected with `error:unsupported_version` and closed. |
| Members' files differ | **Undetected by design.** Playback will look broken with no diagnosis (§11). |

---

## 10. Acceptance criteria

Measured on a LAN first, then with injected RTT/jitter, then Linux-vs-macOS.

- **AC-1** Two clients on the same file: after play + seek + pause + resume, `ahead` stays
  within the deadband (150 ms on LAN) for 60 s.
- **AC-2** A member joining mid-playback converges within 3 s, and **no other member moves**.
- **AC-3** In steady-state playback, each member performs **zero** hard syncs in 60 s and at
  most one nudge entry per 5 s.
- **AC-4** A pause performed by pressing the key *in MPV itself* lands every member on the
  same position within `250 ms + RTT`.
- **AC-5** A **forward** seek by any member — including the group minimum — propagates within
  `RTT + 250 ms` and is **not** reverted by min-wins. (This is the regression test for §5.6.)
- **AC-6** Killing and restarting the server does not interrupt playback; members re-converge
  within 5 s of the server returning.
- **AC-7** With 150 ms ± 50 ms of injected jitter, `ahead` stays within the adaptive deadband
  and the hard-sync rate stays at zero.
- **AC-8** **No cross-host timestamp arithmetic exists**: a grep-level test asserts the source
  contains no peer-supplied time values used in position math (invariant A3).
- **AC-9** A fake member pinned at a fixed position is the group minimum and the group is
  repeatedly dragged back onto it, oscillating by several seconds between rate-limited seeks —
  the documented, expected behaviour of §5.3 and §11.1, asserted so the trade-off cannot
  regress silently.
- **AC-10** Identical behaviour on Linux and macOS.
- **AC-11** 10 members for 10 minutes: no member exceeds one nudge entry per 5 s, and the
  server's CPU stays under a few percent of one core.

---

## 11. Accepted trade-offs

These are consequences of decisions made deliberately during specification. They are
**documented, not mitigated**, so they cannot come back as surprises.

1. **No protection against a genuinely slower member.** With strict min-wins and no outlier
   rejection, a member whose player advances slowly while reporting `playing` becomes a
   permanent group minimum, and everyone is repeatedly pulled back onto it. This is the
   classic "rewind loop". The eligibility rule only excludes members whose position is
   *meaningless* (loading, joining, buffering, seeking). Deliberately chosen over straggler
   demotion or median outlier rejection.

   **The rate limit does not bound the divergence to 1 s.** The hard-sync threshold is 1 s,
   but a member may hard-sync only once per 10 s. Against a straggler advancing at half rate,
   the excess grows by ~0.5 s per second, so after a seek the gap climbs back to **roughly
   5 s** before the next one is permitted — and the 5 % nudge, which is the only thing
   slowing that climb, is itself capped and cannot match a 50 % deficit. So the honest
   statement of the bound is: divergence in this scenario is bounded by the *rate limit*, not
   by the 1 s threshold, and the group will visibly oscillate by several seconds.
2. **No clock synchronization, so accuracy is bounded by the link.** The systematic effect is
   an *under*-estimate of every peer by roughly the peer→receiver one-way latency (§5.1) —
   and because the two legs of the star relay are nearly symmetric, that bias is *common* and
   cancels out of the relative positions that actually matter. What does not cancel is that
   each member sizes its deadband from its *own* RTT, so the member on the slower link
   tolerates the largest offset and can sit ahead of the group by up to about half the
   difference of the two RTTs before correcting itself. A correction also always lands the
   corrector on the peer's *estimated* position, i.e. about that same peer→receiver one-way
   latency below the peer's true position, so corrections undershoot rather than overshoot.
   Absolute accuracy against some "true timeline" is not merely poor, it is undefined:
   nothing in the system defines one.
3. **No file verification at all.** Two members can watch different files and the group will
   mechanically converge onto whichever is behind, with no diagnostic beyond "it feels
   wrong". Each client logs, locally, the MPV command line it spawned — the only place its own
   file path is visible, and therefore the only debugging tool (PROTOCOL §5.1).
4. **Speed is not user-settable inside a session.** Any local speed change is reverted.
5. **Min-wins is one-sided.** A member that is behind is never pulled forward; it either drags
   the group or is excluded for being non-playing. There is no "catch up" path for a behind
   member that is playing.
6. **A pause can seek.** Because a pause snaps the group to the minimum, pausing when the
   group is spread across the deadband causes a small backward seek on the members that are
   ahead.
7. **Joining mid-playback is a visible jump for the joiner** (an immediate exact seek onto the
   group's position), by design, so that the group is never disturbed.
8. **Unauthenticated and unencrypted.** Anyone who can reach the TCP port can drive every
   member's player: pause it, seek it, and — through the rewind-loop behaviour of item 1 —
   pin the group anywhere they like. This is *not* arbitrary code execution: the wire
   protocol carries only `pause`, `resume` and `seek`, and PlaySync2 never forwards an
   arbitrary MPV command such as `run`. It is nevertheless total control over everyone's
   playback, so this is a private-use tool that must not be exposed to the internet.
9. **The server is a single point of availability**, though not of correctness: losing it
   partitions the group silently until it returns.

---

## 12. Out of scope

Unchanged from SPEC v0.1: **rooms** (one server, one session) and **authentication** (private
use). Additionally deferred:

- Media transfer, transcoding, any verification of file content.
- Per-user delay compensation (Bluetooth/AVR latency).
- Subtitle and text synchronisation.
- Windows.
- Config files, systemd units, packaging, GUI.
- Multi-file and playlist synchronisation.
- Multiple concurrent sessions, or any per-session multiplexing on one server.
- Adaptive/partial nudge experiments, drift statistics, session recording.

---

## 13. Decision log

Every question raised during specification, with the answer that produced this document.

| # | Question | Decision |
|---|---|---|
| Q1 | Does the server ever play video? | **No.** Headless event/message dispatcher only. |
| Q2 | Who owns the authoritative clock? | **Nobody.** Distributed min-wins (§5); no server time base, no clock sync. |
| Q3 | Who obtains the MPV instance? | **The client spawns and owns MPV**; users never run MPV directly. |
| Q4 | How is MPV's argv built? | PlaySync2 injects `--input-ipc-server` + `--keep-open=yes` + `--pause=yes`; `--` passthrough for the rest. |
| Q5 | What if MPV exits? | The client exits with it, propagating the status. |
| Q6 | Which positions count toward the minimum? | Only converged, playing members — not joining, not seeking, not buffering, not at EOF, and not silent for more than 5 s. |
| Q7 | Protection against the rewind loop? | **None.** Strict min-wins; the consequence is documented in §11. |
| Q8 | How does an ahead member get back? | Nudge (up to 5 % slower) below 1 s; hard seek back at or above 1 s; rate-limited. |
| Q9 | What does a pause do? | Snaps the whole group to the minimum position, then pauses. |
| Q10 | How does a session start? | Everyone spawns paused at 0; the first `resume` intent starts the group. |
| Q11 | Which actions are synced? | `pause`, `resume`, `seek` only. |
| Q12 | Topology? | Star relay through the server. |
| Q13 | Is the file verified? | **No check at all.** The server is a blind relay. |
| Q14 | A member's connection drops? | Immediate removal; the client re-joins as a fresh member. No grace state. |
| Q15 | Seek versus min-wins? | Explicit seek overrides min-wins and re-bases the group. |
| Q16 | EOF behaviour? | `--keep-open=yes`, and the first EOF broadcasts a group pause at the end. |
| Q17 | Build system? | Plain `Makefile` (`make`, `make test`, `make clean`). |
| Q18 | Thresholds tuning target? | Adaptive: `deadband = min(max(150 ms, rtt_self + 50 ms), 1000 ms)`, where `rtt_self` is an EWMA (α = 0.25) of the last ~10 RTT samples. |
| Q19 | JSON in C? | Vendored `cJSON`. |
| Q20 | Test scope? | Pure-function unit tests + a fake-MPV JSON IPC harness; real-MPV e2e opt-in via `make e2e`. |
| Q21 | Is `speed` a synced setting? | **No** — it is owned by the sync engine and local changes are reverted. |
| Q22 | Do media facts cross the network? | **No.** Not even the file name; the roster shows member names only. |
| Q23 | How is the RTT that sizes the deadband estimated? | EWMA (α = 0.25) of the last ~10 samples — not a window minimum or maximum. |
| Q24 | Reconnect identity? | Fresh UUIDv4 per connection attempt, so a blip re-joins immediately instead of colliding with the server's 15 s liveness reap. |
| Q25 | What actually bounds divergence in the rewind loop? | The 10 s hard-sync interval, **not** the 1 s threshold: against a half-rate straggler the group oscillates by ~5 s. Accepted and documented in §11.1 rather than tuned away, because tuning makes a broken session look tighter while seeking more often. |

---

## 14. Implementation notes

- **Layout:** `src/` (`main.c`, `server.c`, `client.c`, `net.c`, `proto.c`,
  `mpv.c`, `sync.c`, `timebase.c`), `vendor/cJSON/`, `tests/` (`test_sync.c`, `test_proto.c`,
  `fake_mpv.c`), `Makefile`. `timebase.c` wraps the local monotonic clock and nothing else:
  it is not a clock *synchronizer*, per invariant A3.
- **Purity for testability:** the min computation, eligibility rules, extrapolation, and the
  correction state machine are side-effect-free functions over plain structs, taking an
  injected "now". They are the bulk of the unit tests and they never touch a socket.
- **Fake MPV harness:** a small UNIX-socket server that speaks MPV's JSON IPC subset
  (`observe_property`, `get_property`, `set_property pause|speed|time-pos`, `seek`,
  `loadfile`, `quit`) and emits scripted `property-change` events on a simulated timeline.
  It lets the full client run headless in CI. `make e2e` additionally drives real MPV.
- **Scale:** at 2 Hz heartbeats and the 32-member default cap, the server relays at most ~2k
  short messages/second (~8k/s at the hard ceiling of 64), well inside one non-blocking event
  loop.
- **Portability:** `poll(2)` and `clock_gettime(CLOCK_MONOTONIC)` are used on both targets;
  no platform `#ifdef` is expected outside socket-path handling.
