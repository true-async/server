#!/usr/bin/env bash
# Ctrl+C (SIGINT/SIGTERM) delivery harness for TrueAsync (ext/async + http_server).
#
# Reproduction harness for YanGusik/laravel-spawn#8: on macOS the first
# Ctrl+C did not wake `Async\signal(SIGINT)` waiters while the same code
# exits cleanly on Linux.
#
# Each scenario is a standalone PHP script under scenarios/. The runner
# launches it in its own process group (like a terminal foreground job),
# waits for a READY line, then delivers the signal the way a terminal
# does (to the whole group) and separately to the PID only. A scenario
# passes when one signal makes the process exit within 5 seconds.
#
# Scenario header directives (comment lines at the top of the .php file):
#   # SIGNAL: INT|TERM     signal to deliver (default INT)
#   # CONNECT: <port>      hold an open TCP connection to <port> before signaling
#   # MARKER: <string>     log line proving the userland waiter actually woke
#
# Usage: run.sh [/path/to/php] [scenario-name-filter]
set -u

PHP_BIN="${1:-php}"
FILTER="${2:-}"
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${TMPDIR:-/tmp}/ctrlc-harness.$$"
mkdir -p "$OUT_DIR"

# Job control: background jobs get their own process group, matching how
# a terminal runs `php artisan async:dev`.
set -m

RESULTS=""
FAILURES=0

now_alive() { kill -0 "$1" 2>/dev/null; }

# wait_exit <pid> <seconds> -> 0 if the process exited in time
wait_exit() {
    local i=0

    while [ "$i" -lt $(($2 * 10)) ]; do
        now_alive "$1" || return 0
        sleep 0.1
        i=$((i + 1))
    done

    return 1
}

# wait_line <pattern> <file> <seconds> <pid>
wait_line() {
    local i=0

    while [ "$i" -lt $(($3 * 10)) ]; do
        grep -q "$1" "$2" 2>/dev/null && return 0
        now_alive "$4" || return 1
        sleep 0.1
        i=$((i + 1))
    done

    return 1
}

record() { # name verdict
    RESULTS="${RESULTS}$(printf '%-42s %s' "$1" "$2")
"
    case "$2" in
        FAIL*) FAILURES=$((FAILURES + 1)) ;;
    esac
}

run_case() { # <scenario.php> <group|pid>
    local scenario="$1" mode="$2"
    local name sig connect marker log pid pgid client_pid target verdict woke rc

    name="$(basename "$scenario" .php)"
    sig="$(sed -n 's/^# SIGNAL: //p' "$scenario" | head -1)"
    sig="${sig:-INT}"
    connect="$(sed -n 's/^# CONNECT: //p' "$scenario" | head -1)"
    marker="$(sed -n 's/^# MARKER: //p' "$scenario" | head -1)"
    log="$OUT_DIR/$name.$mode.log"

    "$PHP_BIN" -d display_errors=1 "$scenario" > "$log" 2>&1 &
    pid=$!
    pgid="$(ps -o pgid= -p "$pid" | tr -d ' ')"

    if ! wait_line "READY" "$log" 15 "$pid"; then
        if grep -q "^SKIP" "$log" 2>/dev/null; then
            record "$name [$mode]" "SKIP ($(head -1 "$log"))"
        else
            record "$name [$mode]" "BORK (no READY line — see $log)"
            kill -KILL -"$pgid" 2>/dev/null
        fi

        return
    fi

    client_pid=""
    if [ -n "$connect" ]; then
        # Browser-keep-alive analog: a request whose connection stays open,
        # leaving a server coroutine suspended in fread().
        "$PHP_BIN" -r '
            $c = stream_socket_client("tcp://127.0.0.1:'"$connect"'", $e, $m, 5);
            if ($c) { fwrite($c, "GET /hold HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n"); sleep(60); }
        ' > /dev/null 2>&1 &
        client_pid=$!
        sleep 1
    fi

    if [ "$mode" = "group" ]; then
        target="-$pgid"
    else
        target="$pid"
    fi

    kill -s "$sig" -- "$target" 2>/dev/null

    if wait_exit "$pid" 5; then
        wait "$pid" 2>/dev/null
        rc=$?
        verdict="OK (exit=$rc after 1x SIG$sig)"
    elif kill -s "$sig" -- "$target" 2>/dev/null && wait_exit "$pid" 5; then
        wait "$pid" 2>/dev/null
        verdict="FAIL (needed 2x SIG$sig — laravel-spawn#8 behavior)"
    else
        verdict="FAIL (hung after 2x SIG$sig — SIGKILL)"
        kill -KILL -"$pgid" 2>/dev/null
    fi

    # Distinguish "waiter woke but process lingered" from "signal never
    # reached userland" — the key diagnostic split for this bug.
    if [ -n "$marker" ]; then
        if grep -q "$marker" "$log" 2>/dev/null; then
            woke="waiter-woke"
        else
            woke="WAITER-NEVER-WOKE"
        fi
        verdict="$verdict [$woke]"
    fi

    [ -n "$client_pid" ] && kill "$client_pid" 2>/dev/null

    record "$name [$mode]" "$verdict"
    echo "--- $name [$mode]: $verdict"
    sed 's/^/    | /' "$log"
}

echo "PHP: $($PHP_BIN -v 2>/dev/null | head -1)"
echo "Zend Signals: $($PHP_BIN -i 2>/dev/null | grep -i 'zend signal' || echo 'n/a')"
echo "Logs: $OUT_DIR"
echo

for scenario in "$DIR"/scenarios/*.php; do
    case "$(basename "$scenario")" in
        *"$FILTER"*) ;;
        *) continue ;;
    esac

    for mode in group pid; do
        run_case "$scenario" "$mode"
    done
done

echo
echo "================= SUMMARY ================="
printf '%s' "$RESULTS"
echo "==========================================="

if [ "$FAILURES" -gt 0 ]; then
    echo "FAILURES: $FAILURES"
    exit 1
fi

echo "ALL OK"
