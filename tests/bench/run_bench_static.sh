#!/usr/bin/env bash
# Static-file benchmark harness.
#
# Usage: tests/bench/run_bench_static.sh [h1_port] [h2_port]
#
# Boots bench_static_server.php (HTTP/1.1 + h2c prior-knowledge), then
# runs wrk for the H1 path and h2load for the H2 path against four file
# sizes (256 B, 16 KiB, 256 KiB, 8 MiB) plus a 304 If-None-Match probe.
# Assumes the extension is already built against the installed PHP.

set -euo pipefail

H1_PORT="${1:-18090}"
H2_PORT="${2:-18091}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/true_async_server.so"

if [[ ! -f "$EXT" ]]; then
    echo "extension not built at $EXT" >&2
    exit 1
fi

php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
    "$DIR/bench_static_server.php" "$H1_PORT" "$H2_PORT" \
    >/dev/null 2>"$DIR/.static.log" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$DIR/.static.log"' EXIT

# Wait for both ports.
for i in $(seq 1 50); do
    if curl -sf -o /dev/null "http://127.0.0.1:$H1_PORT/static/tiny.txt"; then
        break
    fi
    sleep 0.1
done

if ! curl -sf -o /dev/null "http://127.0.0.1:$H1_PORT/static/tiny.txt"; then
    echo "static bench server did not respond on port $H1_PORT" >&2
    cat "$DIR/.static.log" >&2 || true
    exit 1
fi

# Smoke check: each size is reachable and sized correctly.
declare -A EXPECT=( [tiny.txt]=256 [small.html]=16384 [medium.bin]=262144 [large.bin]=8388608 )
for f in "${!EXPECT[@]}"; do
    got=$(curl -sf "http://127.0.0.1:$H1_PORT/static/$f" | wc -c)
    if [[ "$got" != "${EXPECT[$f]}" ]]; then
        echo "smoke failed: $f returned $got bytes, expected ${EXPECT[$f]}" >&2
        exit 1
    fi
done

DURATION="${BENCH_DURATION:-10s}"
THREADS="${BENCH_THREADS:-2}"
CONNS="${BENCH_CONNS:-50}"

run_h1() {
    local label="$1" path="$2"
    echo
    echo "=== H1 wrk keep-alive  $label  (${THREADS}t/${CONNS}c/${DURATION}) ==="
    wrk -t"$THREADS" -c"$CONNS" -d"$DURATION" "http://127.0.0.1:$H1_PORT$path"
}

run_h2() {
    local label="$1" path="$2" n="$3"
    echo
    echo "=== H2 h2load           $label  (-c10 -m32 -n${n}) ==="
    h2load -c10 -m32 -n"$n" -T5 \
        "http://127.0.0.1:$H2_PORT$path" 2>&1 \
      | grep -E 'finished|req/s|time for request|status codes' || true
}

run_h1 "tiny    256B"   "/static/tiny.txt"
run_h1 "small    16K"   "/static/small.html"
run_h1 "medium  256K"   "/static/medium.bin"
run_h1 "large     8M"   "/static/large.bin"

run_h2 "tiny    256B"   "/static/tiny.txt"   50000
run_h2 "small    16K"   "/static/small.html" 50000
run_h2 "medium  256K"   "/static/medium.bin" 20000
run_h2 "large     8M"   "/static/large.bin"   2000

echo
echo "=== H1 wrk 304 probe (If-None-Match)  small.html ==="
ETAG=$(curl -sI "http://127.0.0.1:$H1_PORT/static/small.html" \
       | awk -F': ' 'tolower($1)=="etag"{ sub(/\r$/,"",$2); print $2 }')
if [[ -n "$ETAG" ]]; then
    wrk -t"$THREADS" -c"$CONNS" -d"$DURATION" \
        -H "If-None-Match: $ETAG" \
        "http://127.0.0.1:$H1_PORT/static/small.html"
else
    echo "no ETag header — skipped"
fi

# Server exits via trap.
