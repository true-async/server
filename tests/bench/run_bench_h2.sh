#!/usr/bin/env bash
# HTTP/2 benchmark harness — REST unary workload (PLAN_HTTP2.md Step 10).
#
# Usage: tests/bench/run_bench_h2.sh [port]
#
# Runs three h2load passes against bench_server.php:
#   1. HTTP/1.1 baseline   (--h1)
#   2. HTTP/2 prior-knowledge, moderate concurrency (c=10 m=10)
#   3. HTTP/2 prior-knowledge, plan-target concurrency (c=100 m=10)
#
# Plan target: H2 within 25% of H1 RPS at c=100 m=10 n=100000.

set -euo pipefail

PORT="${1:-18080}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"

if [[ ! -f "$EXT" ]]; then
    echo "extension not built at $EXT" >&2
    exit 1
fi

if ! command -v h2load >/dev/null 2>&1; then
    echo "h2load not found — install nghttp2-client / nghttp2-tools" >&2
    exit 1
fi

php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
    "$DIR/bench_server.php" "$PORT" >/dev/null 2>"$DIR/.server.log" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$DIR/.server.log"' EXIT

for i in $(seq 1 50); do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
        break
    fi
    sleep 0.1
done
# Give the server loop a beat to settle after the probe — first few
# requests on a cold loop can race against the TCP backlog draining.
sleep 0.3

if ! curl -s "http://127.0.0.1:$PORT/" | grep -q OK; then
    echo "server did not respond on port $PORT" >&2
    cat "$DIR/.server.log" >&2 || true
    exit 1
fi

URL="http://127.0.0.1:$PORT/"
N=100000

echo "=== h2load HTTP/1.1 baseline: -c100 -m1 -n$N --h1 ==="
# --h1 disables multiplex so -m is effectively 1; keep -c100 to compare
# against H2 c=100 total in-flight budget fairly.
h2load -c100 -n"$N" -T2 --h1 "$URL"

echo
echo "=== h2load HTTP/2 prior-knowledge: -c10 -m10 -n$N ==="
h2load -c10 -m10 -n"$N" -T2 "$URL"

echo
echo "=== h2load HTTP/2 prior-knowledge: -c100 -m10 -n$N (plan target) ==="
h2load -c100 -m10 -n"$N" -T2 "$URL"
