#!/usr/bin/env bash
# Benchmark harness.
#
# Usage: tests/bench/run_bench.sh [port]
#
# Starts the bench server in the background, runs two wrk passes
# (connection-close + keep-alive), prints results and kills the server.
# Assumes the extension is already built against the installed PHP.

set -euo pipefail

PORT="${1:-18080}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"

if [[ ! -f "$EXT" ]]; then
    echo "extension not built at $EXT" >&2
    exit 1
fi

# Launch server with our freshly-built extension, detached.
# Use -n to skip system php.ini (same approach as `make test`), and load
# the just-built .so via extension_dir+extension pair so PHP's extension
# loader resolves the filename properly against a trusted path.
php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
    "$DIR/bench_server.php" "$PORT" >/dev/null 2>"$DIR/.server.log" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$DIR/.server.log"' EXIT

# Wait for port to be listening (up to 3 seconds).
for i in $(seq 1 30); do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
        break
    fi
    sleep 0.1
done

# Smoke check.
if ! curl -s "http://127.0.0.1:$PORT/" | grep -q OK; then
    echo "server did not respond on port $PORT" >&2
    cat "$DIR/.server.log" >&2 || true
    exit 1
fi

echo "=== wrk: keep-alive (default), 2 threads, 50 connections, 15s ==="
wrk -t2 -c50 -d15s "http://127.0.0.1:$PORT/"

echo
echo "=== wrk: Connection: close, 2 threads, 50 connections, 15s ==="
wrk -t2 -c50 -d15s -H 'Connection: close' "http://127.0.0.1:$PORT/"

# Server exits via trap.
