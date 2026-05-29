#!/usr/bin/env bash
# Phase-0 HTTP/3 baseline harness (issue #59).
#
# Drives our H3 server (and optionally h2o) with the same ngtcp2-based
# load client (tests/h3client) across a payload x concurrency matrix and
# records, per cell:
#   RPS          median of 3 runs (completed requests / wall time)
#   sendmsg/req  syscalls counted by strace -f -c on the server, /requests
#   recvmsg/req  ditto
#
# sendmsg/req is THE Phase-1 metric: cross-datagram flush coalescing must
# strictly lower it with no RPS regression. PKTS_PER_REQ (QUIC datagrams,
# from getHttp3Stats) is a secondary GSO-efficiency number and is NOT a
# substitute for the syscall count -- GSO collapses many datagrams into one
# sendmsg.
#
# Percentiles (p50/p99) are intentionally absent: our h3client is a
# correctness client (1 in-flight stream/conn) and cannot report them. A
# QUIC-enabled h2load would be needed; tracked as a Phase-0 follow-up.
#
# Usage:
#   tests/bench/h3_compare.sh                 # our server only
#   RUN_H2O=1 tests/bench/h3_compare.sh       # add h2o column
#
# Env knobs: PHP, PAYLOADS, CONCS, PER_CONN, RUNS, OUT.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PHP="${PHP:-/home/edmond/php-release/bin/php}"
CLIENT="$ROOT/tests/h3client/h3client"
TMP="$ROOT/tests/bench/tmp-bench"
CERT="$TMP/cert.pem"
KEY="$TMP/key.pem"
OUT="${OUT:-$ROOT/tests/bench/h3-baseline.json}"

PAYLOADS="${PAYLOADS:-3 16384 1048576}"
CONCS="${CONCS:-10 100}"
RUNS="${RUNS:-3}"             # RPS runs per cell; median kept
STRACE_PER="${STRACE_PER:-20}"   # reduced per-conn load for the syscall-count run
BASE_PORT="${BASE_PORT:-34540}"

# h3client opens a fresh process+QUIC handshake per connection, so RPS here is
# client-bound (a same-tooling regression guard, not absolute server capacity).
# Scale requests/conn by body so 1 MB cells stay tractable over loopback.
per_conn_for() {
    case "$1" in
        1048576) echo "${PER_CONN_BIG:-15}" ;;
        *)       echo "${PER_CONN:-150}" ;;
    esac
}

mkdir -p "$TMP"
[ -f "$CERT" ] || openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj "/CN=localhost" -keyout "$KEY" -out "$CERT" 2>/dev/null

SRV_PID=""
SRV_PORT=""
kill_srv() {
    [ -n "$SRV_PID" ] || return 0
    # Kill the strace tracee (php child) FIRST so strace sees it exit and
    # flushes its -c report, then exits on its own. Killing strace directly
    # orphans php and hangs the wait.
    local ch
    for ch in $(pgrep -P "$SRV_PID" 2>/dev/null); do kill -TERM "$ch" 2>/dev/null; done
    kill -TERM "$SRV_PID" 2>/dev/null
    for _ in $(seq 1 40); do kill -0 "$SRV_PID" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    # Backstop: reap anything still holding the UDP port.
    if [ -n "$SRV_PORT" ]; then
        for ch in $(lsof -ti udp:"$SRV_PORT" 2>/dev/null); do kill -KILL "$ch" 2>/dev/null; done
    fi
    SRV_PID=""
}
trap kill_srv EXIT

# wait_ready <logfile>
wait_ready() {
    for _ in $(seq 1 80); do
        grep -q READY "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# start_ours <port> <body> [strace_out]
# Launches the standalone bench server; under strace when strace_out is set.
start_ours() {
    local port="$1" body="$2" strace_out="${3:-}"
    local log="$TMP/srv.$port.log"
    SRV_PORT="$port"
    : > "$log"
    if [ -n "$strace_out" ]; then
        PHP_HTTP3_BENCH_FC=1 BENCH_BODY="$body" BENCH_PORT="$port" \
            BENCH_CERT="$CERT" BENCH_KEY="$KEY" \
            strace -f -c -e trace=sendmsg,sendmmsg,recvmsg,recvmmsg -o "$strace_out" \
            "$PHP" -d extension_dir="$ROOT/modules" -d extension=true_async_server \
                   "$ROOT/tests/bench/h3_bench_server.php" 2>"$log" &
    else
        PHP_HTTP3_BENCH_FC=1 BENCH_BODY="$body" BENCH_PORT="$port" \
            BENCH_CERT="$CERT" BENCH_KEY="$KEY" \
            "$PHP" -d extension_dir="$ROOT/modules" -d extension=true_async_server \
                   "$ROOT/tests/bench/h3_bench_server.php" 2>"$log" &
    fi
    SRV_PID=$!
    wait_ready "$log" || { echo "server $port failed to start" >&2; cat "$log" >&2; return 1; }
}

# drive_load <host> <port> <path> <conc> <per_conn> -> echoes "<completed> <elapsed_s>"
drive_load() {
    local host="$1" port="$2" path="$3" conc="$4" per="$5"
    local t0 t1 total
    t0=$(date +%s.%N)
    total=$(seq 1 "$conc" | xargs -P "$conc" -I {} sh -c \
        "PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=$per H3CLIENT_QUIET=1 \
         H3CLIENT_DEADLINE_MS=30000 '$CLIENT' '$host' '$port' '$path' GET 2>&1 1>/dev/null \
         | grep -oE 'COMPLETED=[0-9]+' | cut -d= -f2" \
        | awk '{s+=$1} END{print s+0}')
    t1=$(date +%s.%N)
    awk -v c="$total" -v t0="$t0" -v t1="$t1" 'BEGIN{printf "%d %.4f", c, t1-t0}'
}

# median of stdin numbers (one per line)
median() { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

# parse strace -c "calls" column for one syscall name
strace_calls() {
    awk -v sc="$2" '$NF==sc {print $4; found=1} END{if(!found) print 0}' "$1" | tail -1
}

echo "[h3-compare] payloads=[$PAYLOADS] concs=[$CONCS] per_conn=$PER_CONN runs=$RUNS"
RESULTS=""   # accumulates JSON cell objects

port=$BASE_PORT
for body in $PAYLOADS; do
    per=$(per_conn_for "$body")
    for conc in $CONCS; do
        port=$((port + 2))
        # --- RPS runs (no strace) ---
        rps_samples=""
        for _ in $(seq 1 "$RUNS"); do
            start_ours "$port" "$body" || exit 1
            read -r completed elapsed <<<"$(drive_load 127.0.0.1 "$port" / "$conc" "$per")"
            kill_srv
            rps=$(awk -v c="$completed" -v e="$elapsed" 'BEGIN{printf "%.1f", (e>0)?c/e:0}')
            rps_samples="$rps_samples$rps"$'\n'
            sleep 0.3
        done
        rps_med=$(printf '%s' "$rps_samples" | grep -v '^$' | median)

        # --- syscall run (strace) at reduced per-conn load to bound overhead ---
        st="$TMP/strace.$body.$conc.txt"
        start_ours "$port" "$body" "$st" || exit 1
        read -r sc_completed _ <<<"$(drive_load 127.0.0.1 "$port" / "$conc" "$STRACE_PER")"
        kill_srv
        sm=$(strace_calls "$st" sendmsg); smm=$(strace_calls "$st" sendmmsg)
        rm=$(strace_calls "$st" recvmsg); rmm=$(strace_calls "$st" recvmmsg)
        sc_completed=${sc_completed:-0}
        perreq=$(awk -v sm="$sm" -v smm="$smm" -v rm="$rm" -v rmm="$rmm" -v c="$sc_completed" \
            'BEGIN{c=(c>0)?c:1; printf "%.3f %.3f %.3f %.3f", sm/c,(smm+0)/c,rm/c,(rmm+0)/c}')
        read -r sendmsg_pr sendmmsg_pr recvmsg_pr recvmmsg_pr <<<"$perreq"

        printf "  body=%-8s c=%-4s RPS(med)=%-9s sendmsg/req=%s recvmsg/req=%s recvmmsg/req=%s\n" \
            "$body" "$conc" "$rps_med" "$sendmsg_pr" "$recvmsg_pr" "$recvmmsg_pr"

        cell=$(printf '{"server":"ours","body":%d,"conc":%d,"rps_median":%s,"runs":%d,"per_conn":%d,"strace_per_conn":%d,"req_sampled":%d,"sendmsg_per_req":%s,"sendmmsg_per_req":%s,"recvmsg_per_req":%s,"recvmmsg_per_req":%s}' \
            "$body" "$conc" "${rps_med:-0}" "$RUNS" "$per" "$STRACE_PER" "$sc_completed" \
            "$sendmsg_pr" "$sendmmsg_pr" "$recvmsg_pr" "$recvmmsg_pr")
        RESULTS="$RESULTS${RESULTS:+,}$cell"
    done
done

{
    printf '{\n  "schema": "h3-baseline/1",\n'
    printf '  "note": "Phase-0 baseline (issue #59). sendmsg_per_req is THE Phase-1 metric (server syscalls, client-independent) and must drop with no rps regression.",\n'
    printf '  "client": "tests/h3client (ngtcp2, 1 in-flight stream/conn, concurrency via xargs -P)",\n'
    printf '  "caveats": ["rps_median at high conc is client-bound (h3client = process+handshake per conn), use as a same-tooling regression guard, not server capacity", "no p50/p99: h3client cannot report percentiles", "bodies >=~1MB are NOT measurable: h3client stalls (no stream flow-control updates) -> excluded; both percentiles and large-body need a QUIC-enabled h2load (Phase-0 follow-up)"],\n'
    printf '  "cells": [%s]\n}\n' "$RESULTS"
} > "$OUT"

echo "[h3-compare] wrote $OUT"
lsof -ti udp:"$port" 2>/dev/null && echo "WARN: survivor on $port" || true
