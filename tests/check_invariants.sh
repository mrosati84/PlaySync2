#!/bin/sh
# Grep-level invariant checks (SPEC §3.3 A2/A3, AC-8).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
fail=0

no_match() {
    desc="$1"; pattern="$2"; shift 2
    if grep -rnE --include='*.c' --include='*.h' "$pattern" "$@" >/dev/null 2>&1; then
        echo "FAIL: $desc"
        fail=1
    else
        echo "ok: $desc"
    fi
}

# A2: single-threaded, poll(2) based. No threads anywhere.
no_match "no threads" 'pthread_create|<pthread\.h>|std::thread' src
no_match "no select(2)" '\bselect[[:space:]]*\(' src

# A3: the local monotonic clock is wrapped in exactly one place.
hits=$(grep -rn --include='*.c' --include='*.h' 'clock_gettime' src | grep -v '^src/timebase\.c:' || true)
if [ -n "$hits" ]; then
    echo "FAIL: clock is read outside timebase.c:"
    echo "$hits"
    fail=1
else
    echo "ok: clock access isolated to timebase.c"
fi

# A3: no peer-supplied time value is carried on the wire.
no_match "no timestamp field on the wire" '"(ts|timestamp|sent|sent_at|wallclock|epoch)"' src/proto.c

if [ "$fail" = 0 ]; then
    echo "ok: invariants (A2, A3)"
fi
exit "$fail"
