#!/usr/bin/env bash
# HTTP/2 upload-throughput harness (PLAN_HTTP2 Step 10, upload workload).
#
# Usage: tests/bench/run_bench_h2_upload.sh [port] [size_mib]
#
# Generates a sparse-content /tmp file of the requested size once, then
# POSTs it three times:
#   1. HTTP/1.1 baseline (curl --http1.1)
#   2. HTTP/2 prior-knowledge, single stream
#   3. HTTP/2 prior-knowledge, 4 parallel streams (single connection)
#
# Reports wall-clock throughput for each run. Plan target: H2 >= 80 %
# of H1 throughput on the single-stream comparison.

set -euo pipefail

PORT="${1:-18080}"
SIZE_MIB="${2:-1024}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"
PAYLOAD="/tmp/bench_upload_${SIZE_MIB}MiB.bin"

if [[ ! -f "$EXT" ]]; then
    echo "extension not built at $EXT" >&2
    exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "curl not found" >&2
    exit 1
fi

if [[ ! -f "$PAYLOAD" ]] || [[ "$(stat -c %s "$PAYLOAD")" -ne "$((SIZE_MIB * 1024 * 1024))" ]]; then
    echo "Generating $SIZE_MIB MiB payload at $PAYLOAD"
    dd if=/dev/urandom of="$PAYLOAD" bs=1M count="$SIZE_MIB" status=none
fi

# memory_limit=-1: large bodies are accumulated via smart_str (emalloc),
# so PHP's memory_limit must exceed max_body_size or the emalloc fails
# with a fatal inside the parser callback, leaving the reactor in an
# inconsistent state. -1 = unlimited; for production deployments set
# memory_limit to at least 2× setMaxBodySize().
php -d memory_limit=-1 \
    -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
    "$DIR/bench_upload_server.php" "$PORT" >/dev/null 2>"$DIR/.server.log" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$DIR/.server.log"' EXIT

for i in $(seq 1 50); do
    if curl -s -o /dev/null -X POST --data-binary 'probe' "http://127.0.0.1:$PORT/"; then
        break
    fi
    sleep 0.1
done
sleep 0.3

URL="http://127.0.0.1:$PORT/"
SIZE_BYTES=$((SIZE_MIB * 1024 * 1024))

run_one() {
    local label="$1"; shift
    local t0 t1
    # %N gives ns; portable enough on Linux/WSL2.
    t0="$(date +%s%N)"
    "$@" >/dev/null
    t1="$(date +%s%N)"
    local ns=$((t1 - t0))
    local mbs
    mbs=$(awk -v b="$SIZE_BYTES" -v ns="$ns" 'BEGIN{printf "%.2f", (b/1048576.0) / (ns/1e9)}')
    local secs
    secs=$(awk -v ns="$ns" 'BEGIN{printf "%.3f", ns/1e9}')
    echo "  $label: ${secs}s, ${mbs} MiB/s"
}

echo "=== Upload throughput: $SIZE_MIB MiB POST ==="
echo
echo "HTTP/1.1 baseline:"
run_one "curl --http1.1 (single POST)" \
    curl -s --http1.1 -X POST --data-binary "@$PAYLOAD" -o /dev/null "$URL"

echo
echo "HTTP/2 prior-knowledge:"
run_one "curl --http2-prior-knowledge (single stream)" \
    curl -s --http2-prior-knowledge -X POST --data-binary "@$PAYLOAD" -o /dev/null "$URL"

echo
echo "HTTP/2 prior-knowledge, 4 parallel streams on one connection:"
# Body is buffered fully per stream (awaitBody semantics), so 4×SIZE_BYTES
# must fit in RAM. Use SIZE_MIB/4 per stream so aggregate equals the
# single-stream payload; OOM-killer would otherwise shoot the server.
QUARTER_MIB=$((SIZE_MIB / 4))
QUARTER_PAYLOAD="/tmp/bench_upload_${QUARTER_MIB}MiB.bin"
if [[ ! -f "$QUARTER_PAYLOAD" ]] || [[ "$(stat -c %s "$QUARTER_PAYLOAD")" -ne "$((QUARTER_MIB * 1024 * 1024))" ]]; then
    dd if=/dev/urandom of="$QUARTER_PAYLOAD" bs=1M count="$QUARTER_MIB" status=none
fi
QUARTER_BYTES=$((QUARTER_MIB * 1024 * 1024))
t0="$(date +%s%N)"
curl -s --http2-prior-knowledge --parallel --parallel-max 4 \
    -X POST --data-binary "@$QUARTER_PAYLOAD" "$URL" -o /dev/null \
    -X POST --data-binary "@$QUARTER_PAYLOAD" "$URL" -o /dev/null \
    -X POST --data-binary "@$QUARTER_PAYLOAD" "$URL" -o /dev/null \
    -X POST --data-binary "@$QUARTER_PAYLOAD" "$URL" -o /dev/null
t1="$(date +%s%N)"
ns=$((t1 - t0))
total=$((QUARTER_BYTES * 4))
mbs=$(awk -v b="$total" -v ns="$ns" 'BEGIN{printf "%.2f", (b/1048576.0) / (ns/1e9)}')
secs=$(awk -v ns="$ns" 'BEGIN{printf "%.3f", ns/1e9}')
echo "  curl h2 x4 × ${QUARTER_MIB} MiB (aggregate ${SIZE_MIB} MiB): ${secs}s, ${mbs} MiB/s"
