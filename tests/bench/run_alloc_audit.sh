#!/usr/bin/env bash
# Allocation-per-request audit — drives a bench server under the
# malloc_count.so shim and reports the per-request delta for each of
# malloc / calloc / realloc / free.
#
# Usage: tests/bench/run_alloc_audit.sh [h1|h2] [port]
#
# Uses SIGRTMAX (signal 64 on Linux) for snapshots. See
# tests/bench/malloc_count.c for why we avoid SIGUSR1 and SIGRTMIN —
# both are grabbed by PHP / ext/async.

set -euo pipefail

MODE="${1:-h1}"
PORT="${2:-18411}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"
SHIM="$DIR/malloc_count.so"
DURATION="10s"
THREADS=2
CONNS=16

[[ -f "$EXT"  ]] || { echo "extension not built at $EXT" >&2; exit 1; }
[[ -f "$SHIM" ]] || { echo "shim not built at $SHIM (cc malloc_count.c -ldl -lpthread)" >&2; exit 1; }

case "$MODE" in
    h1) SERVER_PHP="$DIR/bench_server.php";    URL="http://127.0.0.1:$PORT/" ;;
    h2) SERVER_PHP="$DIR/h2spec_server.php";   URL="http://127.0.0.1:$PORT/" ;;
    *)  echo "mode must be h1 or h2" >&2; exit 1 ;;
esac

LOG="$DIR/.alloc.log"
: > "$LOG"

# Use `exec` so $! is the php PID rather than a subshell wrapping it.
USE_ZEND_ALLOC=0 LD_PRELOAD="$SHIM" \
    exec php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
    "$SERVER_PHP" "$PORT" >/dev/null 2>"$LOG" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

# Wait for port
for _ in $(seq 1 30); do
    if curl -sk -o /dev/null --max-time 0.3 "$URL"; then break; fi
    sleep 0.1
done

# Warmup (avoid one-off startup allocs in the window)
if [[ "$MODE" == "h1" ]]; then
    wrk -t1 -c4 -d2s "$URL" >/dev/null
else
    h2load -n 200 -c 4 "$URL" >/dev/null 2>&1
fi

# Snapshot: BEFORE
kill -s SIGRTMAX "$SERVER_PID"
sleep 0.2

# Steady-state run
if [[ "$MODE" == "h1" ]]; then
    RUN_OUT=$(wrk -t"$THREADS" -c"$CONNS" -d"$DURATION" "$URL" 2>&1)
    REQS=$(echo "$RUN_OUT" | awk '/Requests\/sec/ {rate=$2} /[[:space:]]requests in/ {print $1}' | head -1)
else
    RUN_OUT=$(h2load -n 100000 -c "$CONNS" -m 10 "$URL" 2>&1 || true)
    REQS=$(echo "$RUN_OUT" | awk '/succeeded/ {for (i=1;i<=NF;i++) if ($i=="succeeded,") print $(i-1)}' | head -1)
fi

# Snapshot: AFTER
kill -s SIGRTMAX "$SERVER_PID"
sleep 0.2

kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

# Parse SNAPSHOT lines from the log
mapfile -t SNAPS < <(grep '^\[malloc_count\] SNAPSHOT' "$LOG" || true)
if [[ "${#SNAPS[@]}" -lt 2 ]]; then
    echo "expected >=2 snapshots, got ${#SNAPS[@]}" >&2
    cat "$LOG" >&2
    exit 1
fi

parse() { echo "$1" | sed -n "s/.*$2=\([0-9]*\).*/\1/p"; }

B_MAL=$(parse "${SNAPS[0]}" malloc)
B_CAL=$(parse "${SNAPS[0]}" calloc)
B_REA=$(parse "${SNAPS[0]}" realloc)
B_FRE=$(parse "${SNAPS[0]}" free)
B_BYT=$(parse "${SNAPS[0]}" bytes)

A_MAL=$(parse "${SNAPS[1]}" malloc)
A_CAL=$(parse "${SNAPS[1]}" calloc)
A_REA=$(parse "${SNAPS[1]}" realloc)
A_FRE=$(parse "${SNAPS[1]}" free)
A_BYT=$(parse "${SNAPS[1]}" bytes)

D_MAL=$(( A_MAL - B_MAL ))
D_CAL=$(( A_CAL - B_CAL ))
D_REA=$(( A_REA - B_REA ))
D_FRE=$(( A_FRE - B_FRE ))
D_BYT=$(( A_BYT - B_BYT ))

if [[ -z "$REQS" || "$REQS" -eq 0 ]]; then
    echo "could not parse request count from runner output" >&2
    echo "$RUN_OUT" >&2
    exit 1
fi

printf "\n=== allocation audit (%s, %s window, %d connections) ===\n" "$MODE" "$DURATION" "$CONNS"
printf "requests in window   : %d\n" "$REQS"
printf "malloc  total / req  : %d / %.2f\n" "$D_MAL"  "$(echo "$D_MAL  / $REQS" | bc -l)"
printf "calloc  total / req  : %d / %.2f\n" "$D_CAL"  "$(echo "$D_CAL  / $REQS" | bc -l)"
printf "realloc total / req  : %d / %.2f\n" "$D_REA"  "$(echo "$D_REA  / $REQS" | bc -l)"
printf "free    total / req  : %d / %.2f\n" "$D_FRE"  "$(echo "$D_FRE  / $REQS" | bc -l)"
printf "alloc bytes / req    : %d / %.0f\n" "$D_BYT"  "$(echo "$D_BYT  / $REQS" | bc -l)"
printf "sum m+c+r / req      : %.2f\n" "$(echo "($D_MAL + $D_CAL + $D_REA) / $REQS" | bc -l)"
