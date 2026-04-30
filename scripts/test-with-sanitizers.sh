#!/bin/bash
# Build with sanitizers + run the full phpt suite under ASAN/UBSAN/LSAN.
# Output goes to /tmp/sanitizer-test.log; non-zero exit on any failure.
#
# Usage:
#   scripts/test-with-sanitizers.sh                # default: all sanitizers
#   SAN=address scripts/test-with-sanitizers.sh    # ASAN only
#   SAN=address,undefined scripts/test-with-sanitizers.sh
#   TESTS=tests/phpt/server/h1/ scripts/test-with-sanitizers.sh
#
# After completion the build is left in a sanitizer state. Run a normal
# `phpize --clean && phpize && ./configure --enable-http2 --enable-http3 &&
# make` to restore the release build.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

SAN="${SAN:-address,undefined}"
TESTS="${TESTS:-tests/phpt/}"
PHP_EXECUTABLE="${PHP_EXECUTABLE:-/usr/local/bin/php}"
LOG="${LOG:-/tmp/sanitizer-test.log}"

# Frankenphp's pattern: USE_ZEND_ALLOC=0 disables Zend's arena so ASAN sees
# every malloc/free; ZEND_TRACK_ARENA_ALLOC keeps trace info for any
# remaining arena-backed allocs. Without these two, ASAN reports almost
# nothing because Zend's per-request reset masks UAFs.
export USE_ZEND_ALLOC=0
export ZEND_TRACK_ARENA_ALLOC=1

# Sanitizer runtime options.
#
# detect_leaks=0: LSAN does not work reliably via LD_PRELOAD against an
#   un-instrumented PHP — it sees the host's pre-existing allocations
#   as leaks. Proper LSAN requires building php-src itself with
#   --enable-address-sanitizer. See docs/PLAN_CI.md note on Step 2.
#   For now ASAN+UBSAN catch UAFs/OOBs/UB; leaks are tracked separately
#   via Valgrind smoke (see tests/bench).
# abort_on_error=1: forces ASAN to abort with core-dump exit code, which
#   the phpt runner detects as test failure (otherwise ASAN's exit 1
#   can be swallowed by the parent).
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:symbolize=1:print_stacktrace=1:halt_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1"

# When PHP itself is not built with ASAN, libasan must be LD_PRELOAD'd
# so the runtime can be initialised at process start. This is the same
# trick frankenphp's sanitizers.yaml does for unmodified PHP. Pick
# the libasan that matches the compiler used to build the .so.
ASAN_LIB="$(gcc -print-file-name=libasan.so 2>/dev/null || true)"
if [ -n "$ASAN_LIB" ] && [ -e "$ASAN_LIB" ]; then
    export LD_PRELOAD="$ASAN_LIB${LD_PRELOAD:+:$LD_PRELOAD}"
fi

echo "[1/2] Building with SAN=$SAN ..."
SAN="$SAN" "$SCRIPT_DIR/build-with-sanitizers.sh" >"$LOG" 2>&1
BUILD_RC=$?
if [ $BUILD_RC -ne 0 ]; then
    echo "BUILD FAILED — see $LOG"
    tail -30 "$LOG"
    exit $BUILD_RC
fi

echo "[2/2] Running phpt suite ($TESTS) under sanitizers..."
echo "      USE_ZEND_ALLOC=$USE_ZEND_ALLOC"
echo "      ASAN_OPTIONS=$ASAN_OPTIONS"
echo "      UBSAN_OPTIONS=$UBSAN_OPTIONS"
echo ""

make PHP_EXECUTABLE="$PHP_EXECUTABLE" test TESTS="$TESTS" 2>&1 | tee -a "$LOG"
TEST_RC=${PIPESTATUS[0]}

echo ""
echo "==================================="
PASS=$(grep -E "^Tests passed" "$LOG" | tail -1 | grep -oE "[0-9]+" | head -1)
FAIL=$(grep -E "^Tests failed" "$LOG" | tail -1 | grep -oE "[0-9]+" | head -1)
echo "Sanitizer suite: pass=${PASS:-?} fail=${FAIL:-?} (SAN=$SAN)"
SAN_ERRORS=$(grep -cE "ERROR: (Address|Leak|Undefined)Sanitizer|runtime error:" "$LOG" || true)
echo "Sanitizer-reported errors in log: $SAN_ERRORS"
echo "==================================="
echo "Full log: $LOG"

if [ "$TEST_RC" -ne 0 ] || [ "${SAN_ERRORS:-0}" -gt 0 ]; then
    echo ""
    echo "FAILURES:"
    grep -B1 -A8 -E "ERROR: (Address|Leak|Undefined)Sanitizer|runtime error:" "$LOG" | head -60
    exit 1
fi
exit 0
