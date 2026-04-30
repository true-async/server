#!/bin/bash
# Build the extension with --enable-coverage, run the full phpt suite,
# generate an lcov HTML report under coverage/html/.
#
# Usage:
#   scripts/coverage.sh              # full run, opens coverage/html/index.html path
#   scripts/coverage.sh --quick      # phpt only (skip C unit tests)
#   scripts/coverage.sh --keep       # do not 'make clean' after; useful for re-runs
#
# Requirements: lcov, genhtml, gcov (apt: lcov), CMocka for unit tests.
# PHP_EXECUTABLE may be overridden; defaults to /usr/local/bin/php.

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

QUICK=0
KEEP=0
for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=1 ;;
        --keep)  KEEP=1 ;;
        *) echo "Unknown arg: $arg" >&2; exit 2 ;;
    esac
done

PHP_EXECUTABLE="${PHP_EXECUTABLE:-/usr/local/bin/php}"
COV_DIR="$PROJECT_DIR/coverage"
RAW_INFO="$COV_DIR/raw.info"
FILT_INFO="$COV_DIR/filtered.info"
HTML_DIR="$COV_DIR/html"

C_RED='\033[0;31m'; C_GRN='\033[0;32m'; C_YEL='\033[1;33m'; C_BLU='\033[0;34m'; C_OFF='\033[0m'
log()  { printf "${C_BLU}[cov]${C_OFF} %s\n" "$*"; }
ok()   { printf "${C_GRN}[ok ]${C_OFF} %s\n" "$*"; }
warn() { printf "${C_YEL}[warn]${C_OFF} %s\n" "$*"; }
die()  { printf "${C_RED}[err]${C_OFF} %s\n" "$*" >&2; exit 1; }

for tool in lcov genhtml gcov "$PHP_EXECUTABLE"; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool not found in PATH"
done

mkdir -p "$COV_DIR"

# 1. Clean rebuild with coverage flags.
log "Reconfiguring with --enable-coverage"
make clean >/dev/null 2>&1 || true
# Strip stale .gcda from previous runs — partial counters cause skew.
find . -name '*.gcda' -delete 2>/dev/null || true

# Preserve user's existing flags from config.nice if present.
EXTRA_FLAGS=""
if [ -f config.nice ]; then
    # Pull --enable-* flags except coverage itself.
    EXTRA_FLAGS=$(grep -oE "'--[a-z][a-z0-9-]*(=[^']*)?'" config.nice \
                  | grep -v -- '--enable-coverage' \
                  | tr '\n' ' ' \
                  | tr -d "'")
fi

log "Configure flags: $EXTRA_FLAGS --enable-coverage"
# shellcheck disable=SC2086
./configure $EXTRA_FLAGS --enable-coverage >/tmp/cov-configure.log 2>&1 \
    || { tail -40 /tmp/cov-configure.log; die "configure failed"; }

log "Building (parallel)"
make -j"$(nproc)" >/tmp/cov-build.log 2>&1 \
    || { tail -40 /tmp/cov-build.log; die "build failed"; }
ok "Build done"

# 2. Capture initial zero-baseline so files never executed still appear.
log "Capturing zero baseline"
lcov --capture --initial \
     --directory "$PROJECT_DIR" \
     --output-file "$COV_DIR/baseline.info" \
     --rc geninfo_unexecuted_blocks=1 \
     --quiet 2>/dev/null

# 3. Run phpt suite.
log "Running phpt suite (PHP_EXECUTABLE=$PHP_EXECUTABLE)"
PHPT_OUT="/tmp/cov-phpt.log"
if ! make PHP_EXECUTABLE="$PHP_EXECUTABLE" test TESTS=tests/phpt/ >"$PHPT_OUT" 2>&1; then
    warn "phpt suite reported failures (continuing — coverage still useful)"
fi
PHPT_PASS=$(grep -E "^Tests passed" "$PHPT_OUT" | tail -1 || echo "Tests passed: ?")
PHPT_FAIL=$(grep -E "^Tests failed" "$PHPT_OUT" | tail -1 || echo "Tests failed: ?")
echo "    $PHPT_PASS"
echo "    $PHPT_FAIL"

# 4. Run C unit tests via CMake/ctest if present and --quick not set.
if [ "$QUICK" -eq 0 ] && [ -d tests/build/unit ]; then
    log "Running C unit tests (cmake)"
    if ( cd tests/build/unit && ctest --output-on-failure ) >/tmp/cov-unit.log 2>&1; then
        ok "C unit tests passed"
    else
        warn "C unit tests reported failures (continuing)"
        tail -30 /tmp/cov-unit.log
    fi
elif [ "$QUICK" -eq 1 ]; then
    log "Skipping C unit tests (--quick)"
else
    warn "tests/build/unit not present; skipping C unit tests"
fi

# 5. Capture post-run counters and merge with baseline.
log "Capturing run counters"
lcov --capture \
     --directory "$PROJECT_DIR" \
     --output-file "$COV_DIR/run.info" \
     --rc geninfo_unexecuted_blocks=1 \
     --quiet 2>/dev/null

lcov --add-tracefile "$COV_DIR/baseline.info" \
     --add-tracefile "$COV_DIR/run.info" \
     --output-file "$RAW_INFO" \
     --quiet 2>/dev/null

# 6. Filter to project sources only — drop deps/, /usr/, system headers, tests.
log "Filtering to src/ (drop deps/llhttp, /usr, tests/)"
lcov --extract "$RAW_INFO" \
     "$PROJECT_DIR/src/*" \
     --output-file "$FILT_INFO" \
     --quiet 2>/dev/null

# 7. Generate HTML report.
log "Generating HTML report"
genhtml "$FILT_INFO" \
        --output-directory "$HTML_DIR" \
        --title "php-http-server coverage" \
        --legend \
        --show-details \
        --sort \
        --quiet 2>/dev/null

# 8. Print per-directory summary using lcov --summary (lcov --list has a
#    known display quirk that shows misleading per-file percentages).
echo ""
log "Per-directory coverage summary:"
echo ""
printf "%-20s %12s %12s\n" "Directory" "Lines" "Functions"
printf -- "----------------------------------------------\n"
TMP_INFO=$(mktemp)
for d in src/core src/formats src/http1 src/http2 src/http3 src; do
    [ -d "$PROJECT_DIR/$d" ] || continue
    lcov --extract "$FILT_INFO" "$PROJECT_DIR/$d/*.c" \
         --output-file "$TMP_INFO" --quiet 2>/dev/null || continue
    L=$(lcov --summary "$TMP_INFO" 2>&1 | grep -oE 'lines\.+: [0-9.]+%' | grep -oE '[0-9.]+%')
    F=$(lcov --summary "$TMP_INFO" 2>&1 | grep -oE 'functions\.+: [0-9.]+%' | grep -oE '[0-9.]+%')
    printf "%-20s %12s %12s\n" "$d" "${L:-?}" "${F:-?}"
done
rm -f "$TMP_INFO"
echo ""
ok "HTML report: file://$HTML_DIR/index.html"
echo "    Raw info: $RAW_INFO"
echo "    Filtered: $FILT_INFO"

# 9. Clean rebuild without coverage so subsequent dev work isn't slowed.
if [ "$KEEP" -eq 0 ]; then
    log "Cleaning coverage build (use --keep to skip)"
    make clean >/dev/null 2>&1 || true
    find . -name '*.gcda' -delete 2>/dev/null || true
    find . -name '*.gcno' -delete 2>/dev/null || true
fi
