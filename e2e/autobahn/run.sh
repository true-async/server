#!/usr/bin/env bash
#
# Run the Autobahn|Testsuite fuzzingclient against the TrueAsync WebSocket
# echo server. Boots server.php, drives the upstream wstest container, then
# grades the JSON report with check_report.php.
#
# Usage:
#   ./run.sh            # smoke: RFC 6455 core cases 1-7 (PR gate)
#   ./run.sh full       # all ~500 cases incl. compression + limits (nightly)
#
# Env:
#   PHP              php binary (default: php)
#   EXT_DIR          directory holding true_async_server.so (default: ../../modules)
#   TRUE_ASYNC       how to load ext/async — empty when it is built statically
#                    into the PHP binary (this repo's dev build); set to
#                    "true_async" or an absolute .so path on shared-module builds
#   WS_PORT          echo-server port (default: 9001)
#   AUTOBAHN_IMAGE   testsuite image (default: crossbario/autobahn-testsuite:0.8.2)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHP="${PHP:-php}"
EXT_DIR="${EXT_DIR:-$HERE/../../modules}"
PORT="${WS_PORT:-9001}"
IMAGE="${AUTOBAHN_IMAGE:-crossbario/autobahn-testsuite:0.8.2}"

# true_async_server is always loaded by absolute path from the build tree.
# ext/async is loaded only when TRUE_ASYNC is set (shared-module builds);
# left empty it is assumed compiled into the PHP binary.
ASYNC_FLAG=()
[ -n "${TRUE_ASYNC:-}" ] && ASYNC_FLAG=(-d extension="$TRUE_ASYNC")

MODE="${1:-smoke}"
case "$MODE" in
    smoke) SPEC="fuzzingclient-smoke.json" ;;
    full)  SPEC="fuzzingclient.json" ;;
    *) echo "usage: $0 [smoke|full]" >&2; exit 2 ;;
esac

echo ">> booting echo server on :$PORT"
"$PHP" -n \
    "${ASYNC_FLAG[@]}" \
    -d extension="$EXT_DIR/true_async_server.so" \
    "$HERE/server.php" "$PORT" &
SRV=$!
cleanup() { kill "$SRV" 2>/dev/null || true; }
trap cleanup EXIT

echo ">> waiting for the server to accept connections"
for _ in $(seq 1 60); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        exec 3>&- 3<&- || true
        break
    fi
    sleep 0.25
done

echo ">> running Autobahn fuzzingclient ($MODE) via $IMAGE"
rm -rf "$HERE/reports"
docker run --rm --network host \
    -v "$HERE:/spec" -w /spec \
    "$IMAGE" wstest -m fuzzingclient -s "/spec/$SPEC"

echo ">> grading report"
"$PHP" "$HERE/check_report.php" "$HERE/reports/servers/index.json"
