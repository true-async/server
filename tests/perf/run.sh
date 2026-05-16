#!/usr/bin/env bash
# Single-worker performance benchmark harness.
#
# Boots one of the perf servers (servers/server_*.php) for each
# combination of protocol (h1 / h2c / h2tls) and benchmark family
# (setbody / static / stream / upload), drives load via wrk (HTTP/1.1)
# or h2load (HTTP/2), prints a per-scenario RPS row, and tears the
# server down between scenarios so concurrent ones don't fight over
# CPU.  Always one server worker — single-threaded numbers.
#
# Usage:
#   tests/perf/run.sh                           # full sweep
#   tests/perf/run.sh setbody                   # only one family
#   tests/perf/run.sh setbody h2tls             # one family, one protocol
#   tests/perf/run.sh -- --c=20 --m=16          # override h2load knobs
#
# Environment:
#   PHP            path to the PHP CLI (default: php)
#   EXT_DIR        path to the extension dir (default: ../../modules)
#   DURATION       per-measurement wall time, seconds  (default: 6)
#   WARMUPS        warmup runs before measurement      (default: 2)

set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd "$DIR/../.." && pwd)"

PHP="${PHP:-php}"
EXT_DIR="${EXT_DIR:-$PROJECT/modules}"
DURATION="${DURATION:-6}"
WARMUPS="${WARMUPS:-2}"

# Family / protocol filters from positional args.
FAMILY_FILTER="${1:-all}"
PROTO_FILTER="${2:-all}"

# Per-family base ports — h1=+0, h2c=+1, h2tls=+2.
declare -A BASE_PORT=(
    [setbody]=19100
    [static]=19110
    [stream]=19120
    [upload]=19130
)

PHP_CMD=("$PHP" -d "extension_dir=$EXT_DIR" -d "extension=true_async_server"
         -d "memory_limit=256M" -d "error_reporting=0")

have() { command -v "$1" >/dev/null 2>&1; }
have h2load || { echo "h2load not found (apt install nghttp2-client)"; exit 1; }
have wrk    || echo "WARNING: wrk not found; h1 benches will be skipped"
have curl   || { echo "curl required for smoke test"; exit 1; }

# --- result table ----------------------------------------------------
RESULTS_FILE="$(mktemp)"
printf '%s\n' "FAMILY|PROTO|SCENARIO|CONC|RPS|MB/s|BODY-SIZE" >"$RESULTS_FILE"
record() {
    printf '%s\n' "$1|$2|$3|$4|$5|$6|$7" >>"$RESULTS_FILE"
}

# --- server lifecycle ------------------------------------------------
start_server() {
    local family="$1" proto="$2" port="$3"
    local script="$DIR/servers/server_${family}.php"

    "${PHP_CMD[@]}" "$script" "$proto" "$port" >/tmp/perf_server.log 2>&1 &
    local pid=$!
    # wait for listener
    for _ in $(seq 1 50); do
        if ss -tln 2>/dev/null | grep -qE "127.0.0.1:$port\b"; then
            echo "$pid"
            return 0
        fi
        sleep 0.1
    done
    echo "ERROR: server $family/$proto did not bind to $port" >&2
    kill "$pid" 2>/dev/null
    cat /tmp/perf_server.log >&2
    return 1
}

stop_server() {
    local pid="$1"
    kill "$pid" 2>/dev/null
    # wait for it to actually go away, free the port
    for _ in $(seq 1 20); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -9 "$pid" 2>/dev/null
}

# --- one measurement -------------------------------------------------
# bench_h2 <url> <c> <m> -> "<rps> <MB/s>"
bench_h2() {
    local url="$1" c="$2" m="$3"
    for _ in $(seq 1 "$WARMUPS"); do
        h2load -c"$c" -m"$m" -D"$DURATION" "$url" >/dev/null 2>&1
    done
    h2load -c"$c" -m"$m" -D"$DURATION" "$url" 2>&1 \
        | awk '/finished in/{
            rps=""; mbs="";
            for (i = 1; i <= NF; i++) {
                if ($i == "req/s,")                      rps = $(i-1);
                if (match($i, /^([0-9.]+)KB\/s/, m))     mbs = m[1] / 1024;
                else if (match($i, /^([0-9.]+)MB\/s/, m)) mbs = m[1];
                else if (match($i, /^([0-9.]+)GB\/s/, m)) mbs = m[1] * 1024;
            }
            print rps, mbs; exit;
          }'
}

# bench_h2_post <url> <body-size-bytes> <c> <m> -> "<rps> <MB/s>"
bench_h2_post() {
    local url="$1" sz="$2" c="$3" m="$4"
    local body; body="$(mktemp)"
    head -c "$sz" /dev/zero >"$body"
    for _ in $(seq 1 "$WARMUPS"); do
        h2load -c"$c" -m"$m" -D"$DURATION" -d "$body" "$url" >/dev/null 2>&1
    done
    h2load -c"$c" -m"$m" -D"$DURATION" -d "$body" "$url" 2>&1 \
        | awk '/finished in/{
            for (i = 1; i <= NF; i++) if ($i ~ /req\/s/) rps = $(i-1);
            for (i = 1; i <= NF; i++) if ($i ~ /MB\/s/)  mbs = $(i-1);
            print rps, mbs; exit;
          }'
    rm -f "$body"
}

# bench_wrk <url> <conns> [post-body-path] -> "<rps> <MB/s>"
bench_wrk() {
    have wrk || { echo "skip skip"; return; }
    local url="$1" c="$2" body="${3:-}"
    local args=(-t 1 -c "$c" -d "${DURATION}s")
    if [ -n "$body" ]; then
        local lua; lua="$(mktemp --suffix=.lua)"
        cat >"$lua" <<EOF
wrk.method = "POST"
local f = io.open("$body", "rb")
wrk.body = f:read("*a"); f:close()
wrk.headers["Content-Type"] = "application/octet-stream"
EOF
        args+=(-s "$lua")
    fi
    for _ in $(seq 1 "$WARMUPS"); do wrk "${args[@]}" "$url" >/dev/null 2>&1; done
    wrk "${args[@]}" "$url" 2>&1 | awk '
        /Requests\/sec/{ rps = $2 }
        /Transfer\/sec/{ mbs = $2 }
        END { print rps+0, mbs }
    '
    [ -n "$body" ] && [ -n "${lua:-}" ] && rm -f "$lua"
}

# Wrap a measurement: filter on family/proto, start server, run sub
# given function $5..., stop server.  Args: family proto port url-prefix
with_server() {
    local family="$1" proto="$2" port="$3"
    shift 3

    [ "$FAMILY_FILTER" = "all" ] || [ "$FAMILY_FILTER" = "$family" ] || return 0
    [ "$PROTO_FILTER" = "all" ]  || [ "$PROTO_FILTER" = "$proto" ]   || return 0
    [ "$proto" = "h1" ] && ! have wrk && return 0

    echo "--- $family / $proto on :$port ---"
    local pid
    pid="$(start_server "$family" "$proto" "$port")" || return 1
    "$@"
    stop_server "$pid"
}

# ---------------------------------------------------------------------
# setBody — buffered REST response
# ---------------------------------------------------------------------
run_setbody() {
    local proto="$1" port="$2"
    local sizes=( "3:b3" "1024:b1k" "16384:b16k" "65536:b64k" )
    # h2tls has a pre-existing bug with body > 64K (initial window stall),
    # so the larger entries are kept only for h2c / h1.
    if [ "$proto" != "h2tls" ]; then
        sizes+=( "262144:b256k" "1048576:b1m" )
    fi
    local scheme="http"; [ "$proto" = "h2tls" ] && scheme="https"

    for spec in "${sizes[@]}"; do
        local sz="${spec%%:*}" label="${spec##*:}"
        local url="$scheme://127.0.0.1:$port/$label"
        for prof in "10:32" "100:10"; do
            local c="${prof%%:*}" m="${prof##*:}"
            local res
            if [ "$proto" = "h1" ]; then
                # h1: wrk, m is irrelevant; use c*m as conn count, capped
                local conns=$(( c * m )); [ $conns -gt 256 ] && conns=256
                res="$(bench_wrk "$url" "$conns")"
            else
                res="$(bench_h2 "$url" "$c" "$m")"
            fi
            local rps="${res% *}" mbs="${res#* }"
            record setbody "$proto" "$label" "c=${c} m=${m}" "$rps" "$mbs" "${sz}B"
        done
    done
}

# ---------------------------------------------------------------------
# static — file delivery
# ---------------------------------------------------------------------
run_static() {
    local proto="$1" port="$2"
    local files=( "256:tiny.txt" "16384:small.html" "262144:medium.bin"
                  "8388608:large.bin" )
    local scheme="http"; [ "$proto" = "h2tls" ] && scheme="https"

    for spec in "${files[@]}"; do
        local sz="${spec%%:*}" name="${spec##*:}"
        local url="$scheme://127.0.0.1:$port/static/$name"
        for prof in "10:32" "100:10"; do
            local c="${prof%%:*}" m="${prof##*:}"
            local res
            if [ "$proto" = "h1" ]; then
                local conns=$(( c * m )); [ $conns -gt 256 ] && conns=256
                res="$(bench_wrk "$url" "$conns")"
            else
                res="$(bench_h2 "$url" "$c" "$m")"
            fi
            local rps="${res% *}" mbs="${res#* }"
            record static "$proto" "$name" "c=${c} m=${m}" "$rps" "$mbs" "${sz}B"
        done
    done
}

# ---------------------------------------------------------------------
# stream — Response->write chunked emission
# ---------------------------------------------------------------------
run_stream() {
    local proto="$1" port="$2"
    # size:chunk pairs — exercises the chunk-ring under different
    # chunk granularities.
    local specs=( "256k:4k" "256k:16k" "1m:16k" "1m:64k" "8m:64k" )
    local scheme="http"; [ "$proto" = "h2tls" ] && scheme="https"

    for spec in "${specs[@]}"; do
        local total="${spec%%:*}" chunk="${spec##*:}"
        local url="$scheme://127.0.0.1:$port/stream/$total/$chunk"
        # streams are heavier — use c=10 m=4 to keep runs short
        local c=10 m=4
        local res
        if [ "$proto" = "h1" ]; then
            res="$(bench_wrk "$url" 40)"
        else
            res="$(bench_h2 "$url" "$c" "$m")"
        fi
        local rps="${res% *}" mbs="${res#* }"
        record stream "$proto" "${total}_${chunk}" "c=${c} m=${m}" "$rps" "$mbs" "$total"
    done
}

# ---------------------------------------------------------------------
# upload — POST body echo (we measure RPS at fixed body size)
# ---------------------------------------------------------------------
run_upload() {
    local proto="$1" port="$2"
    local sizes=( "1024:1K" "16384:16K" "65536:64K" "262144:256K" "1048576:1M" )
    local scheme="http"; [ "$proto" = "h2tls" ] && scheme="https"
    local url="$scheme://127.0.0.1:$port/"

    for spec in "${sizes[@]}"; do
        local sz="${spec%%:*}" label="${spec##*:}"
        for prof in "10:8" "100:4"; do
            local c="${prof%%:*}" m="${prof##*:}"
            local res
            if [ "$proto" = "h1" ]; then
                local conns=$(( c * m )); [ $conns -gt 256 ] && conns=256
                local body; body="$(mktemp)"; head -c "$sz" /dev/zero >"$body"
                res="$(bench_wrk "$url" "$conns" "$body")"
                rm -f "$body"
            else
                res="$(bench_h2_post "$url" "$sz" "$c" "$m")"
            fi
            local rps="${res% *}" mbs="${res#* }"
            record upload "$proto" "$label" "c=${c} m=${m}" "$rps" "$mbs" "${sz}B"
        done
    done
}

# ---------------------------------------------------------------------
# main sweep
# ---------------------------------------------------------------------
echo "Single-worker perf sweep — duration=${DURATION}s warmups=${WARMUPS}"
echo "PHP=$PHP EXT_DIR=$EXT_DIR"
echo "filter: family=$FAMILY_FILTER proto=$PROTO_FILTER"
echo

for proto in h1 h2c h2tls; do
    case "$proto" in
        h1)    off=0 ;;
        h2c)   off=1 ;;
        h2tls) off=2 ;;
    esac

    with_server setbody "$proto" "$(( BASE_PORT[setbody] + off ))" \
        run_setbody "$proto" "$(( BASE_PORT[setbody] + off ))"

    with_server static "$proto" "$(( BASE_PORT[static] + off ))" \
        run_static "$proto" "$(( BASE_PORT[static] + off ))"

    with_server stream "$proto" "$(( BASE_PORT[stream] + off ))" \
        run_stream "$proto" "$(( BASE_PORT[stream] + off ))"

    with_server upload "$proto" "$(( BASE_PORT[upload] + off ))" \
        run_upload "$proto" "$(( BASE_PORT[upload] + off ))"
done

echo
echo "============================ RESULTS ============================"
column -t -s'|' <"$RESULTS_FILE"
rm -f "$RESULTS_FILE"
