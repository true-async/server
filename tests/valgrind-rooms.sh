#!/usr/bin/env bash
#
# The rooms leak lane: the room phpt files under valgrind, with leak checking
# turned into an error so a reference that is taken and never dropped fails the
# run instead of passing quietly.
#
# Why it exists: a hub reference is not observable from PHP. A room that leaks one
# keeps the hub — its slot table, its mutex, its interest filters — alive for the
# life of the process, and every PHP-level assertion still passes. Removing one
# topic_hub_release() from room_free() is caught here and nowhere else.
#
# Two knobs are load-bearing and neither is a run-tests default:
#   USE_ZEND_ALLOC=0  — without it the request allocator serves every PHP-visible
#     allocation out of arenas valgrind cannot see into, so use-after-free and
#     leaks alike disappear.
#   VALGRIND_OPTS     — run-tests wraps the binary in `valgrind -q --tool=memcheck`
#     and nothing more, so leaks are not checked at all. valgrind reads this
#     variable itself, which is how the options get in.
#
# 073 and 074 are deliberately NOT in the list. Both kill a worker with a fatal
# error, and a bailed-out thread leaves php-async's own per-worker request memory
# behind (thread_pool_worker_handler, async_new_scope, resume_when) plus whatever
# the transfer had allocated in that request's arena. Zero definitely-lost is not
# reachable there from this repository; those two are measured by the payload
# balance counter instead (see dev/PLAN.md, S1.9).
#
# Usage: tests/valgrind-rooms.sh [<php binary> [<extension_dir>]]
set -euo pipefail

PHP_BIN="${1:-${TEST_PHP_EXECUTABLE:-php}}"
EXT_DIR="${2:-$(cd "$(dirname "$0")/.." && pwd)/modules}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

USE_ZEND_ALLOC=0 \
VALGRIND_OPTS="--leak-check=full --errors-for-leak-kinds=definite --show-leak-kinds=definite" \
TEST_PHP_EXECUTABLE="$PHP_BIN" \
    "$PHP_BIN" "${RUN_TESTS:-run-tests.php}" \
        -q -m \
        -d extension_dir="$EXT_DIR" \
        -d opcache.protect_memory=0 \
        -g FAIL,BORK,LEAK \
        --no-progress \
        --offline \
        --show-diff \
        --show-mem \
        --set-timeout 300 \
        "${TESTS[@]}"
