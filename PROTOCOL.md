# PlaySync2 — Wire Protocol, Sync Algorithm & MPV Contract

**Protocol version:** 1
**Companion document:** [`SPEC.md`](SPEC.md)
**Verified against:** mpv v0.41.0 on Linux — every behaviour marked *(verified)* was
reproduced on a live instance; see [Appendix A](#appendix-a-verified-mpv-behaviours).

---

## 1. Transport and framing

- **TCP**, one connection per member, client-initiated. Default port **8765**.
- No TLS, no authentication (SPEC §11).
- **Star topology.** Clients talk only to the server; the server relays to every other
  member. There are no direct peer-to-peer connections.
- **Framing:** one JSON object per line, UTF-8, terminated by a single `\n`.
  - Emitters minify; a literal newline never appears inside a message.
  - `\r\n` is accepted on input (tolerant reader, strict writer).
  - **Maximum message: 64 KiB** including framing. Larger → `error:too_large`, then close.
  - The same line reader and JSON writer are reused for MPV's own IPC, which uses identical
    framing.
- **Ordering:** TCP preserves order per connection, and the server relays sequentially, so
  heartbeats and intents from one member always arrive in the order sent. No sequence
  numbers are needed (SPEC invariant A4).
- **Liveness:** the client sends `ping` every 5 s; the server closes a connection idle for
  15 s. Symmetrically, a client that sees no `pong` for 15 s (three missed pings) declares
  the server gone, enters the backoff path, and keeps playing (SPEC §9). A peer whose last
  heartbeat is older than 5 s is treated as ineligible by receivers.
- **Size limits:** reads are chunked through a 16 KiB buffer, and each connection's line
  accumulator grows past it up to the 64 KiB message cap above; `--max-members` defaults
  to 32 and is clamped to a hard ceiling of 64 (SPEC FR-1.2); beyond it the server refuses
  connections with `error{session_full}`.

---

## 2. Message reference

```
C→S   hello     handshake
C→S   hb        heartbeat: this member's position and state
C→S   intent    user action to be adopted by the whole group
C→S   ping      liveness + RTT measurement
C→S   error     member reports a malformed/unknown message from the server (logged)
C→S   bye       graceful leave

S→C   welcome   handshake reply
S→C   roster    member list changed
S→C   hb        relayed heartbeat (adds "from")
S→C   intent    relayed intent (adds "from")
S→C   pong      liveness reply
S→C   error     protocol error
S→C   bye       server-initiated disconnect
```

`error` and `bye` are the only messages that flow in both directions. Without `error` in the
client→server direction, a member that received an unparseable message would have no way to
say so, and rule 5 of §3 would answer a member's `error` with an `error` about the `error`.

### 2.1 `hello` (C→S)

```jsonc
{ "t": "hello",
  "v": 1,                      // protocol version
  "impl": "playsync2/0.1.0",
  "id": "0f0a1c9e-…",          // UUIDv4, generated FRESH for every connection attempt
  "name": "matteo@laptop",
  "observer": false }          // true for --no-mpv
```

No media information is transmitted. Ever. Not the path, not the duration, not the size.

A fresh `id` per connection attempt is what makes SPEC FR-5.4's "returns as a new member"
actually work under rule 2 of §3: reusing an `id` across a reconnect could be rejected as
`duplicate_id` while the server still holds the old, half-open connection — which it only
reaps after 15 s of silence (§1). Generating a new one turns up-to-15-seconds of failed
reconnects into an immediate re-join.

### 2.2 `welcome` (S→C)

```jsonc
{ "t": "welcome",
  "v": 1,
  "session": "3f9c…",          // server instance id; changes when the server restarts.
                               // Informational only — see the note below.
  "you": "0f0a1c9e-…",
  "members": [ { "id": "…", "name": "…", "observer": false } ] }
```

`session` exists purely so that logs from before and after a server restart can be told
apart. Nothing in the algorithm depends on it: a client holds no session state that would
need invalidating, because every reconnection is a fresh member (SPEC FR-5.4).

### 2.3 `hb` — heartbeat (C→S, relayed S→C)

The only source of position information in the system. Emitted at **2 Hz** in every state.

```jsonc
{ "t": "hb",
  "pos": 612.34,               // seconds, or null when no valid position exists
  "state": "playing",          // see the state table below
  "speed": 1.0,                // current playback speed, for peer extrapolation
  "joining": false }           // true until this member has converged (SPEC §5.7)

// relayed form, exactly one field added:
{ "t": "hb", "from": "0f0a1c9e-…", "pos": 612.34, "state": "playing",
  "speed": 1.0, "joining": false }
```

| `state` | Meaning | Eligible? |
|---|---|---|
| `idle` | No file loaded (`idle-active`) | no |
| `loading` | File loading / start-of-file | no |
| `playing` | Playing normally | **yes** |
| `paused` | Paused by the group or by the user | no |
| `buffering` | `paused-for-cache` | no |
| `seeking` | `seeking` is true | no |
| `eof` | `eof-reached` is true | no |

`state` is derived locally by the client from MPV's properties; it is never negotiated.

`pos` is `null` whenever no valid position exists — `idle`, before `file-loaded`, or while
MPV is reporting an unavailable property. A receiver must ignore it; a peer with a null
position can never be in the playing bucket anyway. Sending `null` rather than a fabricated
`0.0` matters: MPV itself omits `data` entirely in exactly this situation (Appendix A), and a
naive receiver would otherwise read a bogus zero as a real position and try to converge the
whole group onto the start of the file.

### 2.4 `intent` (C→S, relayed S→C)

```jsonc
{ "t": "intent", "act": "seek",   "pos": 130.0 }
{ "t": "intent", "act": "pause",  "pos": 120.55 }
{ "t": "intent", "act": "resume", "pos": 120.55 }
```

- `act` is one of `pause`, `resume`, `seek`. Nothing else is ever broadcast.
- `pos` is always the position **actually achieved locally**, not the requested one, except
  for `pause` and `resume`, where it is the minimum over the sender *and* its eligible peers
  (SPEC §5.6) — which is the sender's own position when the sender is that minimum, and its
  own position again when no peer is eligible.
- An intent always overrides the group minimum, in both directions, including a forward seek
  by the member that is currently the minimum (SPEC §5.6). This is the single most important
  rule in the protocol and it is asserted by AC-5.
- `pause` and `resume` are applied *exactly*: every receiver seeks to `pos` and then pauses or
  resumes there, whether or not it is already within the deadband (SPEC §5.6). That exactness
  is what lands the whole group on one frame instead of leaving it spread across the deadband.
- Intents are not queued, not sequenced, and not rate-limited. On a rapid
  pause/resume/pause burst, every member simply applies them in arrival order.
- An unknown `act` is ignored by receivers and never answered. The server does not validate
  intents (§3), so a member must not assume it will only ever see the three actions above —
  and equally, it must not rely on the server to protect it from a malformed one.

### 2.5 `ping` / `pong`

```jsonc
{ "t": "ping", "n": 123 }
{ "t": "pong", "n": 123 }
```

`n` is a client-scoped counter. The client stores its send time **locally** and computes
`rtt = now − sent` on the matching `pong`; no timestamp ever crosses the network.

The client keeps a rolling window of the last ~10 samples and maintains `rtt_self` as an EWMA
over them with α = 0.25. That single number is what the adaptive deadband is built from
(SPEC §5.5), so its estimator is a deliberate choice: an EWMA tracks the link the client
actually has, whereas the window *minimum* would be undersized by one lucky sample and
provoke over-correction, and the window *maximum* would stay wide on a jittery link and leave
real drift uncorrected for too long.

The RTT **is never used as a time base and no clock offset is ever estimated.**

### 2.6 `roster` (S→C)

```jsonc
{ "t": "roster",
  "members": [ { "id": "…", "name": "…", "observer": false } ] }
```

Broadcast on every join and leave. Contains no playback information.

### 2.7 `bye` / `error`

```jsonc
{ "t": "bye", "reason": "mpv_exit" }            // "mpv_exit" | "user_quit" | "shutdown"
{ "t": "error", "code": "session_full", "msg": "…", "fatal": true }
```

Codes: `unsupported_version`, `bad_json`, `unknown_type`, `too_large`, `duplicate_id`,
`session_full`, `internal`. `fatal: true` means the sender is closing.

`internal` is the only code with no protocol trigger: it is for an out-of-band failure in the
sender itself (allocation failure, an unrecoverable I/O error) and is always sent with
`fatal: true` just before the sender goes down.

There is deliberately no `bad_intent`: rule 3 of §3 makes the server a non-validating relay,
so it has no basis on which to reject an intent it never inspects. Malformed intents are the
receiving members' problem, and their rule is to ignore them (§2.4).

---

## 3. Server relay semantics

The server is deliberately trivial. Its entire behaviour:

1. **Accept.** Beyond `--max-members`, refuse with `error{session_full, fatal:true}`.
2. **Handshake.** On `hello`, validate `v`, reject a duplicate `id` with
   `error{duplicate_id, fatal:true}`, bind the connection to `{id, name, observer}`, reply
   `welcome`, and broadcast `roster` to everyone.
3. **Relay.** Every `hb` and `intent` from a connection is forwarded to **every other**
   member with `from` set to the connection's bound id. Any `from` supplied by the client is
   discarded. No other field is read, validated, or modified.
4. **Answer.** `ping` → `pong`. Nothing else is generated.
5. **Reject the unparseable.** A line that is not JSON gets `error{bad_json}`; a message
   whose `t` the server does not recognise gets `error{unknown_type}`. Neither is fatal and
   neither disconnects (SPEC §9) — the server simply has nothing to relay. An `error` *from* a
   member is logged and never answered, so a member reporting a bad server message cannot
   provoke an error reply loop. The one exception is during the handshake, where an unusable
   `hello` closes the connection.
6. **Liveness.** Close any connection with no traffic for 15 s; broadcast `roster`.
7. **Never.** Never store a position, never keep a timeline, never timestamp a message,
   never compare members, never inspect a payload's meaning. The last-traffic timestamp in
   (6) is the only clock the server has, and it refers to nothing but the socket.

There is consequently **no server-side recovery logic, no session state, and nothing to
restore** after a restart beyond the roster itself.

---

## 4. The min-wins algorithm

This runs identically in every client, at 4 Hz, using only local time.

### 4.1 Peer state

```
peer { id, pos0, speed, state, joining, observer, t_recv }
                                                 // t_recv is a LOCAL monotonic timestamp
                                                 // observer is learned from the roster (§2.6)
```

Updated on every relayed `hb` from that peer.

### 4.2 Classifying peers

Every peer is placed in at most one bucket, derived from its most recent heartbeat:

| Bucket | Membership | Used for |
|---|---|---|
| **Playing** | `state == "playing"`, not an observer, not joining, heartbeat ≤ 5 s old | the group minimum, and therefore every drift correction |
| **Static** | `state` is `"paused"` or `"eof"`, not an observer, not joining, heartbeat ≤ 5 s old | locating a group that is not playing, so a joining member can land on it |
| **Ignored** | `idle`, `loading`, `seeking`, `buffering`, observers, joiners, stale heartbeats | nothing |

Only positions that *advance* are extrapolated; static positions are used raw. A member
always excludes itself from both buckets.

### 4.3 The group references

```
group_min(now):                                     # lowest *playing* position
    m = undefined
    for each peer p in PLAYING bucket (p != self):
        cand = p.pos0 + p.speed * (now - p.t_recv)  # extrapolate forward
        m = min(cand, m)
    return m

group_static():                                     # where a non-playing group sits
    return min over STATIC bucket of p.pos0

group_state():                                      # "playing" | "paused" | "unknown"
    if PLAYING bucket is non-empty: return "playing"
    if STATIC  bucket is non-empty: return "paused"
    return "unknown"
```

- **Extrapolation is the whole trick.** A peer's position is advanced by *local elapsed time
  since receipt*, so no clock synchronization, no offset, and no shared epoch is required
  (SPEC invariant A3). The peer→self one-way latency is never measured, and the candidate is
  therefore a slight *under*-estimate: it claims the peer is a little further behind than it
  really is. That biases `ahead` slightly *upward*, so a member corrects marginally more than
  it strictly must. Over-correction keeps the group tight, the bias is common to every member
  (so relative sync survives), and the deadband (SPEC §5.5) is sized to exceed it.
- **Bounded staleness.** A peer that pauses is only excluded from its *next* heartbeat, so for
  at most one heartbeat period (500 ms) its candidate keeps advancing on paper. That is a
  transient under-correction, never an over-correction, and it self-heals on the next
  heartbeat.
- **Undefined is a valid answer.** If no peer is in the playing bucket, `group_min` is
  undefined and the client performs no correction at all.

The **group position** (SPEC §2) is `group_min` while `group_state()` reports `"playing"`,
and `group_static` while it reports `"paused"`; it is undefined when the state is `"unknown"`.
It is what a joining member converges onto (§4.4), and the only place a position is ever
taken from a *non-playing* peer.

### 4.4 The 4 Hz tick

```
local monotonic `now`; local rtt_self, state, joining, local_pos, speed_applied

deadband = min( max(150 ms, rtt_self + 50 ms), 1000 ms )   # SPEC §5.5
hard     = 1 s

on_tick():                                   # 4 Hz, in every state
    if observer or local_pos == null:        # nothing to control and nothing to compare
        return
    if joining:                              # catch-up; runs even while paused
        switch group_state():
            "playing":
                m = group_min(now)
                if m == undefined:
                    within_since = now     # nothing to compare against yet
                elif |local_pos - m| > deadband:
                    cmd_seek(m)            # immediate, exact
                    within_since = now     # the one-second clock restarts after every seek
                elif now - within_since >= 1 s:
                    joining = false
                cmd_pause(false)           # adopt the group's play state
            "paused":
                s = group_static()
                if |local_pos - s| > deadband:
                    cmd_seek(s); within_since = now
                elif now - within_since >= 1 s:
                    joining = false
                cmd_pause(true)
            "unknown":
                joining = false            # nothing to converge on
        return

    # --- not joining ---
    if state != "playing":                   # covers seeking, buffering, eof, paused, idle
        restore_speed_if_nudging(); return

    m = group_min(now)
    if m == undefined: restore_speed_if_nudging(); return

    ahead = local_pos - m

    if ahead >= hard and now - last_hard >= 10 s:
        cmd_seek(m); last_hard = now; cmd_speed(1.0)

    else if ahead > deadband and now - last_nudge_transition >= 5 s:
        cmd_speed(1 - clamp(ahead / 10 s, 0.01, 0.05)); last_nudge_transition = now

    else if ahead <= deadband / 2:
        cmd_speed(1.0)
```

Properties this encodes, all of them deliberate:

- **An observer, or a member with no position, does nothing at all.** The tick returns
  immediately for `--no-mpv` members and whenever `local_pos` is `null` (§2.3). Without that
  guard the catch-up branch would happily send `cmd_seek` to an MPV instance that does not
  exist — reachable as soon as an observer joins a session that is already playing.
- **Catch-up runs while paused.** A joining member is normally paused (everyone spawns
  paused), so gating catch-up on `state == "playing"` would leave it stuck at its own start
  position forever, and the first play it performed would broadcast a position of 0 and drag
  the whole group to the beginning of the file.
- **Convergence needs one unbroken second.** `within_since` is initialised when `joining`
  becomes true, and is reset by every seek *and* by every tick in which the member sits
  outside the deadband — so a member that keeps overshooting cannot clear `joining` on the
  strength of a stale timestamp.
- **`state` already encodes seeking, buffering and EOF** (§2.3), so the non-playing guard is a
  single test rather than a list of flags. That branch still issues exactly one command while
  not playing: restoring `speed` to 1.0, so an interrupted correction can never leave a
  member permanently slowed.
- **A joining member originates no intent but still applies received ones.** Its local
  play/pause/seek are adopted from the group and it emits nothing until it clears `joining`
  (SPEC §5.7). This is what closes the hole above.
- **One-sided.** Only `ahead > 0` is ever acted on. A member that is behind is never seeked
  forward; it is the minimum and the others come down to it.
- **Never transmitted.** `cmd_seek` and `cmd_speed` are *corrections*, not intents. They must
  not be announced, or the group would ping-pong (SPEC §5.5).
- **Hysteresis.** Corrections start at `deadband` and stop at `deadband / 2`.
- **Bounded nudge.** At most 5 % slower; a 1 s excess takes up to ~20 s to shed.
- **Bounded violence.** At most one hard seek per member per 10 s, and at most one nudge
  *entry* per 5 s (leaving a nudge — restoring `speed` to 1.0 — is never rate-limited, since
  it is always safe). This is what keeps a pathological minimum (SPEC §11) from becoming a
  seek storm — at the cost that the drift such a minimum causes is allowed to accumulate to
  several seconds between the seeks that undo it. SPEC §11.1 quantifies that bound.
- **Never stale.** A peer silent for more than 5 s stops counting.

### 4.5 Reference implementation

```
static bool fresh(const struct peer *p, double now)
{
    return !p->observer && !p->joining && now - p->t_recv <= 5.0;
}

static double group_min(const struct peer *peers, int n, double now)
{
    double m = -1.0;                       /* -1 == undefined */
    for (int i = 0; i < n; i++) {
        const struct peer *p = &peers[i];
        if (!fresh(p, now) || p->state != ST_PLAYING) continue;
        double cand = p->pos0 + p->speed * (now - p->t_recv);
        if (cand < 0.0) continue;
        if (m < 0.0 || cand < m) m = cand;
    }
    return m;
}

static double group_static(const struct peer *peers, int n, double now)
{
    double s = -1.0;                       /* -1 == undefined */
    for (int i = 0; i < n; i++) {
        const struct peer *p = &peers[i];
        if (!fresh(p, now)) continue;
        if (p->state != ST_PAUSED && p->state != ST_EOF) continue;
        if (p->pos0 < 0.0) continue;
        if (s < 0.0 || p->pos0 < s) s = p->pos0;
    }
    return s;
}
```

Pure: no sockets, no globals, no clock of their own — `now` is injected. The `peers` array
never contains the calling member itself (§4.3's `p != self`), so neither function needs a
self check. These two functions plus the tick above are the primary unit-test surface, and
exactly what the fake-MPV harness exercises end to end.

---

## 5. MPV JSON IPC contract

### 5.1 Spawning MPV

The client builds MPV's argv as **the user's passthrough first, then our own injected
options**, so that our options cannot be clobbered by the user's config or flags:

```
argv = [ "mpv" ] ++ everything_after("--")
                 ++ [ "--input-ipc-server=" SOCK,
                      "--keep-open=yes",
                      "--pause=yes" ]
```

- `SOCK` is `$XDG_RUNTIME_DIR/playsync2-<pid>.sock`, falling back to
  `/tmp/playsync2-<pid>.sock`. Any existing file at that path is unlinked first, and the
  socket is removed on every exit path, including signal handlers (`SIGINT`, `SIGTERM`,
  `SIGHUP` forward to MPV, then cleanup).
- The path is kept short on purpose: `sockaddr_un` is limited to ~104 bytes on macOS.
- `--pause=yes` is what makes every member start **paused** at frame 0 (SPEC §4, Q10); like
  the other injected options it goes last, so neither the user's flags nor `mpv.conf` can
  clobber it.
- PlaySync2 never parses the passthrough. It does not need to know which argument is the
  media file, and it never transmits it: it only logs the command line it spawned, locally
  and verbatim (SPEC §11.3), which is where that client's own file path is visible.
- The client waits up to 2 s for the socket to accept connections after spawn; then it
  registers observers and waits for `file-loaded`. If the socket never appears — mpv failed to
  start, wrong binary, an mpv built without IPC support — the client kills mpv and exits
  non-zero with a one-line reason (SPEC FR-1.7) rather than continuing half-attached.

### 5.2 Observers

Registered immediately after connecting, before anything else. Observer ids are client-local
and fixed for the lifetime of the MPV connection.

| id | Property | Purpose |
|---|---|---|
| 1 | `pause` | play/pause state; local pause intent detection |
| 2 | `time-pos` | position for heartbeats and drift; local seek detection |
| 3 | `seeking` | suppress drift correction while seeking, and tell a user seek from one of ours (§5.5) |
| 4 | `eof-reached` | EOF policy (SPEC §5.8) |
| 5 | `paused-for-cache` | buffering state |
| 6 | `speed` | nudge bookkeeping; revert of unauthorised local changes |
| 7 | `idle-active` | distinguishes "no file" from "paused at 0" |
| 8 | `playlist-pos` | detects that MPV moved to another playlist entry |

With `--no-mpv` there is no MPV process, no IPC connection, and therefore no observers at
all: the client runs the network side only, reports `pos: null` and `state: "idle"` forever,
and never appears in anyone's buckets (§4.2).

Deliberate non-choices:

- **`audio-pts` is not used.** It updates faster than once per frame when audio is present,
  but it is a different quantity from `time-pos`; mixing two position sources for no accuracy
  we need (the deadband already absorbs frame quantisation) is not worth the ambiguity.
- **`path` and `duration` are not observed.** No media information is used or transmitted.
- `time-pos/full` is not needed: `time-pos` already reports six decimals; it is the *frame
  quantisation* that limits resolution, not the formatting.

### 5.3 Commands

| Purpose | Command |
|---|---|
| Pause / resume | `{"command":["set_property","pause",true]}` |
| Nudge speed | `{"command":["set_property","speed",0.97]}` |
| Seek | `{"command":["seek",130.0,"absolute"]}` |
| Seek while paused | `{"command":["set_property","time-pos",130.0]}` |
| Overlay (opt-in) | `{"command":["show-text","drift -0.02s"]}` |

Every request carries a `request_id`. **A reply is matched by `request_id` only.** The MPV
manual documents that the socket is not serviced while a command executes, so unrelated
events may be interleaved before the reply — and this was reproduced live.

A command is issued only when the desired value differs from the last known value for that
property, to avoid generating a spurious `property-change` that the classifier would then
have to suppress.

### 5.4 Events the client consumes

`property-change` (by observer id), `seek`, `playback-restart`, `start-file`, `file-loaded`,
`end-file`. Others are ignored, not errors.

Only `property-change`, `seek`, `start-file` and `file-loaded` drive decisions (§5.5).
`playback-restart` merely re-arms the seek classifier, and `end-file` with reason `quit` is
the client's signal that MPV is going away and it is about to exit with it (SPEC FR-1.5).

### 5.5 Classifying a local user action

Any local change — keybinding, OSC, script, another IPC client — arrives as a
`property-change`, which is why **no key remapping is required**.

| Observation | Classification |
|---|---|
| `pause` → value we commanded, within the pending deadline | confirmation; no traffic |
| `pause` → any other value | **intent: pause/resume** |
| `time-pos` jumped by more than 1 s during a seek (`seeking` set), with no command or correction of ours in flight | **intent: seek** at the observed position |
| `time-pos` jumped as a result of our own command or correction | internal; no traffic |
| `seek` event with no command or correction in flight, **and** a `time-pos` delta beyond 1 s | **intent: seek** at the position reported by the following `time-pos` |
| `speed` → value we did not command | logged, then **reverted** to the engine value (SPEC §5.9) |
| `eof-reached` → true | **intent: pause** at the minimum over the sender and its eligible peers (SPEC §5.8) |
| `start-file` / `playlist-pos` changed | re-enter `loading` and the `joining` catch-up phase |

The last row covers an MPV playlist advance. **v0.1 supports a single media file**; if the
user passes several, only the first is synchronised, and a playlist advance puts that member
back into the catch-up phase rather than being misread as a seek.

The `seek`-event row carries a second condition for a reason: MPV documents that the `seek`
event also fires for *internal* seeks the user never asked for (ordered chapters, Matroska
segment changes). The event alone is therefore not evidence of a user action, and it must be
corroborated by a `time-pos` jump that no command and no correction of ours can explain.
Without that pairing, an ordered-chapters file would broadcast a spurious seek every time mpv
changed segment.

### 5.6 Echo suppression and pending intents

Each property the client drives keeps a pending record `{value, deadline}` with
`deadline = max(500 ms, 3 × rtt_self)`:

- A `property-change` matching a pending value is consumed silently.
- On expiry, the client re-asserts its intended value once and logs a warning if it is not
  confirmed again.
- The originating member of an intent **never re-applies its own intent on receipt**. It
  already performed the action; re-applying would stutter the playback it just produced.

| Intent | Pending records set when a member applies the intent |
|---|---|
| `seek` | `time-pos` pending at `pos` |
| `pause` / `resume` | `pause` pending at `true` / `false`, plus `time-pos` pending at `pos` (a `pause` or `resume` always seeks exactly — SPEC §5.6) |

The originator sets these when it performs the action locally, before sending; a receiver sets
them when it applies the intent on arrival. Neither path re-broadcasts the resulting
`property-change`.

### 5.7 Edge cases that are explicitly handled

- **A pause caused by `--keep-open` at EOF** must not be broadcast as a user pause. Every
  `pause` change is disambiguated against `eof-reached` before classification.
- **`time-pos` is frame-quantised** *(verified)*, so sub-frame differences are not drift; the
  deadband sits above it.
- **A second IPC client** driving the same MPV is tolerated: all changes are observed
  uniformly, and the only thing PlaySync2 insists on owning is `speed`.
- **`--keep-open=yes` cannot be clobbered**: our options are appended last, and command-line
  options take precedence over `mpv.conf`. Should that precedence ever change, EOF would exit
  MPV and the client would exit with it — a defined behaviour, warned about in `--verbose`,
  not a crash.
- **Relative keyframe seeks** (MPV's arrow keys) land on keyframes; the client always
  reports and relays the *achieved* position, which it reads back from `time-pos`.

---

## 6. Client state machines

**Session state** (network side):

```
CONNECTING ──hello/welcome──► JOINING(joining=true)
     ▲                              │  converged with the group reference, or no peers
     │ lost                         ▼
  BACKOFF ◄──────────────────► CONVERGED(joining=false)
```

`JOINING` and `CONVERGED` are *session* states, and they must not be confused with bucket
*eligibility* (§4.2), which is a property that **other** members infer from each heartbeat. A
`CONVERGED` member that is buffering is still converged; it is simply counted in nobody's
minimum while it reports a non-playing `state`.

`BACKOFF` is the state SPEC §9 names the local `partitioned` status: the client keeps playing
undisturbed, performs no correction while it has no peer heartbeats (the group minimum is
undefined), and retries with exponential backoff. It is purely local — nothing about it is a
wire state.

**Playback state** (MPV side), derived from the observers and reported verbatim in `hb`:

| From | To | Trigger |
|---|---|---|
| `idle` | `loading` | a file starts loading (`start-file`) |
| `loading` | `paused` | `file-loaded`, and MPV starts paused (we pass `--pause=yes`, §5.1) |
| `paused` | `playing` | `pause` → false |
| `playing` | `paused` | `pause` → true |
| `playing` ⇄ `seeking` | | the `seeking` property going true/false |
| `playing` ⇄ `buffering` | | `paused-for-cache` going true/false |
| `playing` | `eof` | `eof-reached` → true (with `--keep-open=yes`) |
| any | `idle` + `loading` | `start-file` / `playlist-pos` changed (§5.5) |

`seeking` and `buffering` are transient overlays rather than steady states: they are reported
as their own `state` value precisely because they must be ineligible, and the member returns
to `playing` when the condition clears.

**Local correction state:** `in_sync` ⇄ `nudging` (speed below 1.0) ⇄ `hard_syncing`
(transient, while `seeking` is true).

The two sides are independent: the session state describes the *network*, the playback state
describes *MPV*, and neither constrains the other.

---

## 7. Versioning

- `v` is an integer, currently `1`.
- Adding a field or a message type does not bump it; unknown fields are ignored and unknown
  message types produce `error{unknown_type}` without disconnecting.
- Removing, renaming, or changing the meaning of anything bumps it.
- A `hello` with an unsupported `v` gets `error{unsupported_version, fatal:true}` and the
  connection closes. There is no downgrade negotiation.

---

## Appendix A: verified MPV behaviours

Reproduced on **mpv v0.41.0**, Linux, with:

```
mpv --no-config --vo=null --ao=null --idle=yes \
    --input-ipc-server=/tmp/probe.sock --really-quiet &

# then drive it from a persistent Python client holding ONE socket open for the whole
# session. A fresh `socat` invocation per command cannot be used to test observation at
# all, because closing the connection destroys the IPC client and unregisters its
# observers (see the last two rows of this table):
#
#   send {"command":["observe_property",1,"time-pos"]}
#   send {"command":["observe_property",2,"pause"]}
#   send {"command":["loadfile","test.mp4","replace"]}
#   send {"command":["set_property","pause",true]}
#   send {"command":["seek",20,"absolute"]}
#   send {"command":["get_property","time-pos"],"request_id":7}
```

| Behaviour | Evidence / consequence for PlaySync2 |
|---|---|
| `--input-ipc-server` creates a UNIX socket that accepts multiple simultaneous clients, and **all** clients receive **all** events | PlaySync2 can coexist with the user's own scripts; but a second PlaySync2 instance would fight for `speed` |
| `observe_property` produces an **immediate** `property-change` on registration, with `data` **omitted entirely** when the property is unavailable | "no `data`" must be read as *unknown*, never as `null` or `0`; this is how the client knows a file is not loaded yet |
| `time-pos` property-change fires **once per decoded video frame** (66.7 ms apart on a 15 fps file) | the tick loop must be throttled to 4 Hz rather than driven by this event |
| `time-pos` is quantised to frame boundaries (values stepped `0.066667`) | frame quantisation is absorbed by the deadband |
| `seek 20 absolute` replies immediately, then emits `seek` → `property-change time-pos` → `seeking:true` → `playback-restart` → `seeking:false` | the classifier must wait for the `seeking` edge before reading the achieved position |
| `set_property time-pos 55` works even while paused and behaves as a seek | the paused seek path in §5.3 |
| `set_property pause true` emits `property-change pause`; `core-idle` flips with it | `core-idle` is unusable as a "loaded" signal; `idle-active` is used instead |
| with `--keep-open=yes`, EOF sets `pause:true` and holds the last frame, `eof-reached:true`, `time-pos` frozen | the reason EOF disambiguation exists (§5.7) |
| `set_property speed 1.02` is accepted (`get_property speed` → 1.02) | nudging is feasible |
| `--audio-pitch-correction` defaults to **on**, inserting scaletempo2 whenever speed differs from 1.0 | a ±5 % nudge is inaudible in pitch |
| `seek` defaults to `exact` for absolute targets; `--hr-seek=absolute` is the option-level default | intents produce frame-accurate seeks without extra flags |
| closing one client's connection unregisters **only** that client's observers | the PlaySync2↔MPV connection is stateful and long-lived, and is never re-established: a fatal socket error terminates the client (SPEC §9), so there is no reconnect blind spot during which drift could be mis-computed |
| the socket is **not serviced while a command executes**, so unrelated events can precede a reply | replies must be matched by `request_id`, never by stream position |

---

## Appendix B: worked trace

Two members A and B, plus C joining late. `──►` is the server relay path: every member-to-
member message goes `X ──► server ──► everyone else`. There are no direct peer connections.

```
1) BOOTSTRAP
   A: hello ──► server ──► welcome(session, members=[A]); roster to all
   A spawns mpv paused at 0: state="loading" until file-loaded, then state="paused"
   A: hb{pos:0.00, state:"paused", joining:true}   (2 Hz)
   A is alone, so the very first tick finds no peers, group_state = "unknown" and clears
   joining (SPEC §5.7 rule 5) — within 250 ms of start

2) B JOINS
   B: hello ──► welcome(members=[A,B]) ──► roster to A
   B spawns mpv paused at 0
   A sees B's hb, but B is joining → B is in neither bucket → A's minimum is unchanged
   B's tick: group_state = "paused" (A is in the static bucket at 0.00)
             group_static = 0.00, |0 - 0.00| <= deadband → after 1 s B clears joining
             B also adopts pause(true), which it already is

3) FIRST PLAY  (A presses play in mpv)
   A: pause property-change false, unexpected → intent{resume, pos:0.00}
   A ──► server ──► B,  (A does not re-apply its own intent)
   B: seeks to 0.00 (a resume always seeks exactly), then pause=false
   both now state="playing"; both eligible; both heartbeats at 2 Hz

4) STEADY STATE
   A at 100.02, B at 100.00, deadband 150 ms
   A's minimum = B's 100.00 (+ extrapolation) → ahead = 0.02 s → inside deadband → no action
   B's minimum = A's 100.02 → ahead = -0.02 s → not ahead → no action
   (nobody is ever pulled forward)

5) DRIFT
   B's disk hiccups; B falls to 98.60 while A is at 100.00
   A's minimum = B's 98.60 → A is ahead by 1.40 s ≥ 1 s → A hard-seeks to 98.60
   (one band lower: if A were only 0.60 s ahead it would instead set
    speed = 1 − clamp(0.60/10, 0.01, 0.05) = 0.95 and leave seeking alone)

6) B SEEKS FORWARD  (the min-wins conflict, resolved)
   B presses the right-arrow key, lands at 130.00
   B: unexpected time-pos jump with seeking=true → intent{seek, pos:130.00}
   B ──► server ──► A
   A: seeks to 130.00 exactly, and does NOT consult its minimum
   (had min-wins taken precedence, A at 98.60 would have been the minimum and B would
    have been dragged back to 98.60 — the explicit-seek exception prevents this)

7) C JOINS LATE  (mid-playback, the case the catch-up rules exist for)
   A and B are playing near 130.00; C spawns paused at 0 with joining:true
   C's heartbeats are excluded by A and B → neither A nor B moves
   C's tick: group_state = "playing", group_min = 130.0x
             |0 - 130.0x| > deadband → C seeks to 130.0x, and cmd_pause(false)
             after 1 s within the deadband, C clears joining and starts counting
   Note the alternative that was designed out: if C stayed a passive non-converged member,
   the first play C performed would carry pos = min(0, group_min) = 0 and drag the whole
   group back to the start of the file. While joining, C originates no intent at all.

8) PAUSE  (C presses space in mpv)
   C's position 130.10, A 130.05, B 130.02; C is ahead
   C: pos = min(130.10, group_min=130.02) = 130.02 → C seeks to 130.02 and pauses
   C: intent{pause, pos:130.02} ──► A, B
   A and B: seek to 130.02 (a pause always seeks exactly), then pause → all three on the same
   frame
   all three now report state="paused" and are counted in nobody's minimum

9) RESUME
   A: intent{resume, pos:130.02} ──► everyone plays from 130.02

10) EOF
   B reaches the end first; mpv sets pause=true by itself with eof-reached=true
   B: the pause is disambiguated as EOF → intent{pause, pos}
   B applies the ordinary pause rule (SPEC §5.6): min(B's own position = the end,
   group min over the still-playing peers A and C) = the lower of A and C
   A, C: seek onto pos and pause; B seeks back onto it too → the group stops together
```

---

## Appendix C: fake-MPV harness

For CI, `tests/fake_mpv.c` implements the subset of MPV's JSON IPC that PlaySync2 uses:

- accepts a UNIX-socket connection at a path given on its command line;
- implements `observe_property`, `unobserve_property`, `get_property`, `set_property`
  (`pause`, `speed`, `time-pos`), `seek`, `loadfile`, `show-text`, `quit`;
- replies with the same shapes MPV does (`{"error":"success","request_id":N}`,
  `{"event":"property-change","id":N,"name":"…","data":…}`), including the immediate
  `property-change` on registration and the omission of `data` for unavailable properties;
- advances a **simulated timeline** from a virtual clock, so tests run at any speed and are
  not wall-clock dependent;
- can be scripted to inject the edge cases: EOF with `keep-open`, a stall
  (`paused-for-cache`), an unexpected local pause, a keyframe-quantised seek, a playlist
  advance, and (for AC-9) a peer pinned at a fixed position.

This is what makes the min-wins algorithm, eligibility, the correction state machine, and
the intent classifier testable without MPV, a display server, or real time.
