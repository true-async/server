#!/bin/bash
# Build PHP HTTP Server extension with sanitizers enabled.
#
# Default: AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer.
# Override via SAN env, e.g. `SAN=address scripts/build-with-sanitizers.sh`
# or `SAN=address,undefined`.
#
# IMPORTANT: callers must run the resulting extension with USE_ZEND_ALLOC=0,
# otherwise PHP's arena allocator hides UAFs/OOBs from ASAN. The companion
# `scripts/test-with-sanitizers.sh` sets that env var automatically.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

SAN="${SAN:-address,leak,undefined}"
SAN_CFLAGS="-fsanitize=$SAN -fno-omit-frame-pointer -g -O1"
SAN_LDFLAGS="-fsanitize=$SAN"

# Match the project's normal configure flags. Without --enable-http2 /
# --enable-http3 the H2/H3 source files do NOT get built and the
# sanitizer pass would silently skip the bulk of the code under test.
EXTRA_FLAGS="${HTTP_SERVER_CONFIGURE_FLAGS:---enable-http2 --enable-http3}"

echo "==================================="
echo "Building with Sanitizers ($SAN)"
echo "  configure flags: $EXTRA_FLAGS"
echo "==================================="

echo "[1/4] Cleaning previous build..."
[ -f Makefile ] && make clean || true
phpize --clean || true

echo "[2/4] Running phpize..."
phpize

echo "[3/4] Configuring..."
# shellcheck disable=SC2086
./configure \
    $EXTRA_FLAGS \
    CFLAGS="$SAN_CFLAGS" \
    LDFLAGS="$SAN_LDFLAGS"

echo "[4/4] Building..."
make -j"$(nproc)"

echo ""
echo "Built modules/http_server.so with sanitizer=$SAN"
echo ""
echo "Run the test suite via:"
echo "  scripts/test-with-sanitizers.sh"
echo ""
echo "Or manually with the required env:"
echo "  USE_ZEND_ALLOC=0 ZEND_TRACK_ARENA_ALLOC=1 \\"
echo "    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:symbolize=1 \\"
echo "    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \\"
echo "    make PHP_EXECUTABLE=/path/to/php test"
