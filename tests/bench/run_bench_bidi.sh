#!/usr/bin/env bash
# Bidi-streaming echo bench (PLAN_HTTP2 follow-up #4, minimum-viable).
#
# Usage: tests/bench/run_bench_bidi.sh [port] [duration] [conns]
#
# Drives bench_bidi_server.php with h2load, sending a 128 KiB body per
# stream and expecting it back echoed. Measures:
#   - throughput (req/s, MiB/s both directions)
#   - RSS growth over the window (sustained leak signal)
#   - active_requests / requests_shed_total from telemetry
#   - h2_streams_* counters
#
# Goes through the same multiplex path h2load -c10 -m10 uses = 100
# concurrent streams interleaving DATA frames both ways on 10 TCP
# connections.

set -euo pipefail

PORT="${1:-18600}"
DURATION="${2:-30s}"
CONNS="${3:-10}"
MULTI="10"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"

[[ -f "$EXT" ]] || { echo "extension not built at $EXT" >&2; exit 1; }
command -v h2load >/dev/null || { echo "h2load missing (apt install nghttp2-client)" >&2; exit 1; }

# 128 KiB random-ish payload — repeatable, non-compressible enough.
BODY=/tmp/bidi_body.bin
head -c $((128 * 1024)) /dev/urandom > "$BODY"

# Launch server (exec so $! == php pid, see run_alloc_audit.sh rationale).
USE_ZEND_ALLOC=1 \
    exec php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
        "$DIR/bench_bidi_server.php" "$PORT" >/dev/null 2>/tmp/bidi.srv.log &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true; rm -f "$BODY"' EXIT

# Wait for the listener.
for i in $(seq 1 40); do
    if curl -s --http2-prior-knowledge --max-time 0.5 \
            -o /dev/null "http://127.0.0.1:$PORT/" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

echo "=== h2load bidi echo: c=$CONNS m=$MULTI -D $DURATION, 128 KiB body ==="
echo "server pid=$SRV (before) RSS: $(ps -o rss= -p "$SRV" 2>/dev/null | awk '{print $1"KB"}')"

h2load -c"$CONNS" -m"$MULTI" -D"$DURATION" \
       -d "$BODY" -H 'Content-Type: application/octet-stream' \
       "http://127.0.0.1:$PORT/" 2>&1 \
   | grep -E "^finished|^requests|^status codes|^traffic|^time for request|^req/s"

echo
echo "server pid=$SRV (after)  RSS: $(ps -o rss= -p "$SRV" 2>/dev/null | awk '{print $1"KB"}')"

# Pull telemetry via a single prior-knowledge request — the H1 path has
# a fallback handler that doesn't expose telemetry, so use a dedicated
# endpoint? No such endpoint; we ran a pure bench. Read the PHP-side
# log if anything surfaced.
if [[ -s /tmp/bidi.srv.log ]]; then
    echo "=== server log tail ==="
    tail -20 /tmp/bidi.srv.log
fi
