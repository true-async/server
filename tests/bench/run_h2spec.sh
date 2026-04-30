#!/usr/bin/env bash
#
# h2spec compliance harness (Step 9).
#
# Spins up a plaintext h2c server, runs summerwind/h2spec against it,
# captures results. Designed to be re-runnable so baseline drift is
# obvious in CI.
#
# Usage: tests/bench/run_h2spec.sh [port]
# Output: results printed to stdout + written to h2spec-results.txt.
# Exit 0 only if every test outside the documented XFAIL list passes.

set -uo pipefail

PORT="${1:-18081}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"
EXT="$PROJECT/modules/http_server.so"
RESULTS="$DIR/h2spec-results.txt"

if [[ ! -f "$EXT" ]]; then
    echo "extension not built at $EXT" >&2
    exit 1
fi

H2SPEC="${H2SPEC:-h2spec}"
if ! command -v "$H2SPEC" >/dev/null 2>&1; then
    if [[ -x "$HOME/.local/bin/h2spec" ]]; then
        H2SPEC="$HOME/.local/bin/h2spec"
    else
        echo "h2spec not installed. See tests/bench/README.md" >&2
        exit 2
    fi
fi

# Launch server in the background — same pattern as run_bench.sh.
php -d extension_dir="$PROJECT/modules" -d extension=true_async_server \
    "$DIR/h2spec_server.php" "$PORT" >/dev/null 2>"$DIR/.h2spec-server.log" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

# Give the server a moment to bind.
sleep 0.3

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "server failed to start, see $DIR/.h2spec-server.log" >&2
    cat "$DIR/.h2spec-server.log" >&2
    exit 3
fi

echo "Running h2spec against 127.0.0.1:$PORT ..."
echo ""

# h2spec prior-knowledge: -k disables TLS, we're testing h2c.
# --strict enforces the "MUST" RFC rules.
"$H2SPEC" \
    --host 127.0.0.1 \
    --port "$PORT" \
    --timeout 5 \
    --strict \
    2>&1 | tee "$RESULTS"
RC=${PIPESTATUS[0]}

echo ""
echo "Results written to $RESULTS"
exit "$RC"
