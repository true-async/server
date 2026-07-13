#!/usr/bin/env bash
#
# Drive the WebSocket topic server with a client we did not write.
#
# Every other topic test speaks to a hand-rolled RFC 6455 client that lives in
# this repo, so a framing habit we got wrong on BOTH sides would pass. The Python
# `websockets` library is an independent implementation and negotiates
# permessage-deflate by default, so the publishes it receives are compressed
# frames decoded by code that has never seen ours.
#
# Usage:
#   ./run.sh
#
# Env:
#   PHP        php binary (default: php). Needs the zlib extension, or the
#              server cannot offer permessage-deflate.
#   EXT_DIR    directory holding true_async_server.so (default: ../../modules)
#   WS_PORT    server port (default: 9101)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHP="${PHP:-php}"
EXT_DIR="${EXT_DIR:-$HERE/../../modules}"
PORT="${WS_PORT:-9101}"

if ! python3 -c 'import websockets' 2>/dev/null; then
    echo "python3 'websockets' package is required: pip install websockets" >&2
    exit 1
fi

"$PHP" -d extension="$EXT_DIR/true_async_server.so" -d protect_memory=0 \
       "$HERE/server.php" "$PORT" &
server=$!

cleanup() { kill -9 "$server" 2>/dev/null || true; }
trap cleanup EXIT

# Four workers have to bind before the first client connects.
sleep 4

python3 "$HERE/client.py" "$PORT"
