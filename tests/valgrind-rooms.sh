#!/usr/bin/env bash
#
# The rooms leak lane: the connectionless room phpt files under valgrind, with
# leak checking turned into an error so a reference that is taken and never
# dropped fails the run instead of passing quietly.
#
# Why it exists: a hub reference is not observable from PHP. A room that leaks one
# keeps the hub — its slot table, its mutex, its interest filters — alive for the
# life of the process, and every PHP-level assertion still passes. Removing one
# topic_hub_release() from room_free() is caught here and nowhere else.
#
# VALGRIND_OPTS is what makes it a leak lane, and it is not something run-tests
# can be asked for: `-m` wraps the binary in `valgrind -q --tool=memcheck
# --trace-children=yes` and no leak options at all, so with -q memcheck prints
# nothing and run-tests' "the log file is non-empty" test is never true. valgrind
# reads the variable itself. (USE_ZEND_ALLOC=0 matters just as much, but run-tests
# already exports it for every valgrind run; a hand-run valgrind needs it too.)
#
# WHAT THIS LANE DOES NOT SEE. A body still pointed at from a leaked structure is
# "indirectly lost" or plain reachable, not "definitely lost": removing the ring
# drop from the dead-request sweep stays green here under every leak-kind filter.
# That class is measured by the body balance instead — ws_bodies against
# ws_bodies_freed, asserted by tests 074 and 078 — which does redden for it.
#
# WHAT IS IN THE LIST: the room tests that need no client connection. The rest of
# the room suite (058-067) stands up a real server and a WebSocket client, which
# is a different kind of run and a much longer one under memcheck; the retry queue
# and the connection-side subscription paths are therefore NOT under leak
# surveillance here. 073 and 074 are left out for a reason of their own: both kill
# a worker with a fatal error, and a bailed-out thread leaves php-async's own
# per-worker request memory behind (thread_pool_worker_handler, async_new_scope,
# resume_when), so zero definitely-lost is not reachable there from this
# repository.
#
# Usage: tests/valgrind-rooms.sh [<php binary> [<extension_dir>]]
set -euo pipefail

PHP_BIN="${1:-${TEST_PHP_EXECUTABLE:-php}}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT_DIR="${2:-$ROOT/modules}"

TESTS=(
    tests/phpt/websocket/068-room-outlives-server.phpt
    tests/phpt/websocket/069-room-transfers-to-pool.phpt
    tests/phpt/websocket/070-room-receive-in-pool.phpt
    tests/phpt/websocket/071-room-recv-contracts.phpt
    tests/phpt/websocket/072-room-recv-no-timeout.phpt
    tests/phpt/websocket/075-room-loss-across-threads.phpt
    tests/phpt/websocket/076-room-one-body-per-publish.phpt
    tests/phpt/websocket/077-room-send-reports-reach.phpt
    tests/phpt/websocket/078-room-detach-every-exit.phpt
)

cd "$ROOT"

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# -j1: run-tests parallelises by default whenever more than one file is named and
# caps at 2 under valgrind, which on a 2-core runner puts two memcheck'd ZTS
# processes with thread pools against each other and their own recv() deadlines.
# Serial is a few seconds slower and does not flake.
# No --set-timeout: run-tests hardcodes 300 s per test under valgrind.
set +e
USE_ZEND_ALLOC=0 \
VALGRIND_OPTS="--leak-check=full --errors-for-leak-kinds=definite --show-leak-kinds=definite" \
TEST_PHP_EXECUTABLE="$PHP_BIN" \
    "$PHP_BIN" "${RUN_TESTS:-run-tests.php}" \
        -q -m -j1 \
        -d extension_dir="$EXT_DIR" \
        -d opcache.protect_memory=0 \
        -g FAIL,BORK,LEAK \
        --no-progress \
        --offline \
        --show-diff \
        --show-mem \
        "${TESTS[@]}" 2>&1 | tee "$LOG"
STATUS=${PIPESTATUS[0]}
set -e

# A lane that runs nothing passes: run-tests exits 0 when every test SKIPs, and
# `die("Cannot find test file …")` on a mistyped path exits 0 as well. Count what
# actually ran.
PASSED=$(sed -n 's/^Tests passed *: *\([0-9]*\).*/\1/p' "$LOG" | tail -1)

if [ "$STATUS" -ne 0 ]; then
    echo "Leak lane: run-tests exited $STATUS" >&2
    exit "$STATUS"
fi

if [ "${PASSED:-0}" -ne "${#TESTS[@]}" ]; then
    echo "Leak lane: expected ${#TESTS[@]} tests to pass, saw '${PASSED:-none}' — nothing was measured" >&2
    exit 1
fi
