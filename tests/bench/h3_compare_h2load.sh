#!/usr/bin/env bash
# Phase-0 HTTP/3 baseline harness, h2load edition (issue #59).
#
# Drives the standalone H3 bench server with a real QUIC load generator
# (the ngtcp2-built h2load) and records, per (body x conc x streams) cell:
#   rps_median   median of RUNS runs (req/s, from h2load)
#   p50 / p99    request latency (from h2load's "request :" line)
#   sendmsg/req  server sendmsg counted by strace -f -c, / requests done
#                (THE Phase-1 coalescing metric — client-independent)
#
# Whatever .so is in modules/ is measured; run once per build to A/B a change.
#
# IMPORTANT: h2load runs with -t = -c. With -t 1 and several connections,
# h2load starves connections in its single event loop and produces spurious
# multi-second p99 stalls that look like a server bug but are not. See
# docs/H3_BENCHMARKING.md.
#
# Usage:  OUT=/tmp/h3-h2load.json tests/bench/h3_compare_h2load.sh
# Env:    PHP, H2LOAD, PAYLOADS, CONCS, STREAMS, NREQ, RUNS, STRACE_NREQ, OUT
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PHP="${PHP:-/home/edmond/php-release/bin/php}"
H2LOAD="${H2LOAD:-/home/edmond/nghttp2/src/h2load}"   # libtool wrapper (in-tree libnghttp2)
TMP="$ROOT/tests/bench/tmp-bench"
CERT="$TMP/cert.pem"; KEY="$TMP/key.pem"
OUT="${OUT:-$ROOT/tests/bench/h3-baseline-h2load.json}"

PAYLOADS="${PAYLOADS:-3 16384}"
CONCS="${CONCS:-1 10}"
STREAMS="${STREAMS:-1 32}"
NREQ="${NREQ:-4000}"
RUNS="${RUNS:-3}"
STRACE_NREQ="${STRACE_NREQ:-2000}"
PORT=$((34800 + RANDOM % 200))

mkdir -p "$TMP"
[ -f "$CERT" ] || openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes \
    -subj "/CN=localhost" -keyout "$KEY" -out "$CERT" 2>/dev/null

SRV=""; SP=""
# Kill the tracee child first so a strace tracer flushes its -c report and
# self-exits; killing the tracer first orphans php and hangs the wait.
kill_srv() {
    [ -n "$SRV" ] || return 0
    local ch
    for ch in $(pgrep -P "$SRV" 2>/dev/null); do kill -TERM "$ch" 2>/dev/null; done
    for _ in $(seq 1 50); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
    kill -TERM "$SRV" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$SRV" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null

    if [ -n "$SP" ]; then
        for ch in $(lsof -ti udp:"$SP" 2>/dev/null); do kill -KILL "$ch" 2>/dev/null; done
    fi

    SRV=""
}
trap kill_srv EXIT

# start_srv <body> [strace_out] — fresh port each call
start_srv() {
    PORT=$((PORT + 2)); SP=$PORT
    local body="$1" strace_out="${2:-}" log="$TMP/h2l.$PORT.log"
    : > "$log"
    if [ -n "$strace_out" ]; then
        PHP_HTTP3_BENCH_FC=1 BENCH_BODY="$body" BENCH_PORT="$PORT" BENCH_CERT="$CERT" BENCH_KEY="$KEY" \
            strace -f -c -e trace=sendmsg,sendmmsg,recvmsg,recvmmsg -o "$strace_out" \
            "$PHP" -d extension_dir="$ROOT/modules" -d extension=true_async_server \
                   "$ROOT/tests/bench/h3_bench_server.php" 2>"$log" &
    else
        PHP_HTTP3_BENCH_FC=1 BENCH_BODY="$body" BENCH_PORT="$PORT" BENCH_CERT="$CERT" BENCH_KEY="$KEY" \
            "$PHP" -d extension_dir="$ROOT/modules" -d extension=true_async_server \
                   "$ROOT/tests/bench/h3_bench_server.php" 2>"$log" &
    fi
    SRV=$!
    for _ in $(seq 1 80); do grep -q READY "$log" 2>/dev/null && { sleep 0.4; return 0; }; sleep 0.1; done
    echo "server $PORT failed to start" >&2; cat "$log" >&2; return 1
}

# load <conc> <streams> <nreq> — drives the current $PORT; output to $TMP/h2l.out
load() {
    timeout 90 "$H2LOAD" --alpn-list=h3 -t "$1" -m "$2" -c "$1" -n "$3" \
        "https://127.0.0.1:$PORT/" > "$TMP/h2l.out" 2>&1
}
rps_of()  { grep -oE 'finished in [^,]*, [0-9.]+ req/s' "$TMP/h2l.out" | grep -oE '[0-9.]+ req/s' | grep -oE '[0-9.]+'; }
done_of() { grep -oE '[0-9]+ done' "$TMP/h2l.out" | grep -oE '[0-9]+' | head -1; }
p50_of()  { awk '/^request /{print $5}' "$TMP/h2l.out"; }
p99_of()  { awk '/^request /{print $7}' "$TMP/h2l.out"; }
median()  { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

echo "[h3-h2load] payloads=[$PAYLOADS] concs=[$CONCS] streams=[$STREAMS] n=$NREQ runs=$RUNS (-t = -c)"
RESULTS=""
for body in $PAYLOADS; do
  for conc in $CONCS; do
    for m in $STREAMS; do
      rps_samples=""; p50=""; p99=""
      for _ in $(seq 1 "$RUNS"); do
          start_srv "$body" || exit 1
          load "$conc" "$m" "$NREQ"; kill_srv
          rps_samples="$rps_samples $(rps_of)"; p50=$(p50_of); p99=$(p99_of)
          sleep 0.3
      done
      rps_med=$(echo "$rps_samples" | tr ' ' '\n' | grep -E '[0-9]' | median)

      st="$TMP/h2l.strace.$body.$conc.$m.txt"
      start_srv "$body" "$st" || exit 1
      load "$conc" "$m" "$STRACE_NREQ"; dn=$(done_of); kill_srv
      sm=$(awk '$NF=="sendmsg"{print $4;f=1}END{if(!f)print 0}' "$st")
      rmm=$(awk '$NF=="recvmmsg"{print $4;f=1}END{if(!f)print 0}' "$st")
      dn=${dn:-0}
      smpr=$(awk -v s="$sm" -v c="$dn" 'BEGIN{print (c>0)?s/c:0}')
      rmmpr=$(awk -v s="$rmm" -v c="$dn" 'BEGIN{print (c>0)?s/c:0}')

      printf "  body=%-8s c=%-3s m=%-3s rps=%-10s p50=%-7s p99=%-7s sendmsg/req=%-7s recvmmsg/req=%s\n" \
          "$body" "$conc" "$m" "${rps_med:-0}" "${p50:-?}" "${p99:-?}" "$smpr" "$rmmpr"
      cell=$(printf '{"body":%d,"conc":%d,"streams":%d,"rps_median":%s,"p50":"%s","p99":"%s","req_sampled":%d,"sendmsg_per_req":%s,"recvmmsg_per_req":%s}' \
          "$body" "$conc" "$m" "${rps_med:-0}" "${p50:-?}" "${p99:-?}" "$dn" "$smpr" "$rmmpr")
      RESULTS="$RESULTS${RESULTS:+,}$cell"
    done
  done
done

{
  printf '{\n  "schema": "h3-baseline-h2load/1",\n'
  printf '  "client": "h2load (nghttp2 1.70, QUIC/ngtcp2), -t = -c, -m = streams/conn",\n'
  printf '  "note": "sendmsg_per_req is the Phase-1 coalescing metric; high -m exercises it. See docs/H3_BENCHMARKING.md.",\n'
  printf '  "cells": [%s]\n}\n' "$RESULTS"
} > "$OUT"
echo "[h3-h2load] wrote $OUT"
