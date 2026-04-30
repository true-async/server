#!/usr/bin/env bash
# Multi-worker benchmark harness — measures REUSEPORT scaling.
#
# Usage: tests/bench/run_bench_multithread.sh [port] [workers...]
#   port:    TCP port to bind (default 18080)
#   workers: one or more worker counts to run in sequence (default: 1 2 4)
#
# For each worker count, starts bench_multithread_server.php with that
# many workers, runs a 15s keep-alive wrk pass, kills the server, and
# prints the result.

set -euo pipefail

PORT="${1:-18080}"
shift || true
WORKERS=("$@")
if [[ ${#WORKERS[@]} -eq 0 ]]; then
    WORKERS=(1 2 4)
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"

if [[ ! -f "$EXT" ]]; then
    echo "extension not built at $EXT" >&2
    exit 1
fi

run_one() {
    local n="$1"
    local log="$DIR/.server.multi.log"

    php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
        "$DIR/bench_multithread_server.php" "$PORT" "$n" \
        >/dev/null 2>"$log" &
    local pid=$!
    # Kill children recursively (pool workers live in the same pgid).
    trap "kill -TERM $pid 2>/dev/null; wait $pid 2>/dev/null || true; rm -f '$log'" RETURN

    # Wait up to 3s for port to start accepting.
    for i in $(seq 1 30); do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
            break
        fi
        sleep 0.1
    done

    if ! curl -s "http://127.0.0.1:$PORT/" | grep -q OK; then
        echo "server (workers=$n) did not respond on port $PORT" >&2
        cat "$log" >&2 || true
        return 1
    fi

    echo "=== workers=$n, keep-alive, 4 threads, 100 connections, 15s ==="
    wrk -t4 -c100 -d15s "http://127.0.0.1:$PORT/"
    echo
}

for n in "${WORKERS[@]}"; do
    run_one "$n"
done
