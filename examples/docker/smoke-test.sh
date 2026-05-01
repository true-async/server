#!/usr/bin/env bash
# In-container smoke test: starts multi-worker.php in the background, hits
# every endpoint, validates keep-alive + pipelining + multi-worker dispatch,
# runs a short wrk burst, then stops.

set -euo pipefail

PORT="${PORT:-8080}"
TLS_PORT="${TLS_PORT:-8443}"
H3_PORT="${H3_PORT:-$TLS_PORT}"
TLS_CERT="${TLS_CERT:-/certs/server.crt}"
TLS_KEY="${TLS_KEY:-/certs/server.key}"
WORKERS="${WORKERS:-0}"   # 0 => auto via available_parallelism()
H3CLIENT="${H3CLIENT:-/app/h3client}"
LOG="$(mktemp)"

TLS_AVAILABLE=0
[[ -r "$TLS_CERT" && -r "$TLS_KEY" ]] && TLS_AVAILABLE=1

echo ">> WORKERS=$WORKERS (0 = auto) PORT=$PORT TLS_PORT=$TLS_PORT TLS=$TLS_AVAILABLE"
WORKERS="$WORKERS" PORT="$PORT" TLS_PORT="$TLS_PORT" H3_PORT="$H3_PORT" \
TLS_CERT="$TLS_CERT" TLS_KEY="$TLS_KEY" \
    php /app/multi-worker.php >"$LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    echo ">> server log tail:"
    tail -n 5 "$LOG" | sed 's/^/   /'
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 50); do
    curl -fsS "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && { ready=1; break; }
    sleep 0.1
done
[[ "$ready" == 1 ]] || { echo "server failed to start" >&2; exit 1; }

grep -E "cpu sources|workers ·" "$LOG" | sed 's/^/   /'

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1: $2" >&2; exit 1; }
expect() {
    local label="$1" want="$2" got="$3"
    [[ "$got" == "$want" ]] && pass "$label" || fail "$label" "want [$want] got [$got]"
}

expect "GET /"                "ok"   "$(curl -fsS "http://127.0.0.1:$PORT/")"
expect "GET /pipeline"        "ok"   "$(curl -fsS "http://127.0.0.1:$PORT/pipeline")"
expect "GET /baseline sum"    "6"    "$(curl -fsS "http://127.0.0.1:$PORT/baseline?a=1&b=2&c=3")"
expect "POST /baseline body"  "15"   "$(curl -fsS -X POST -d '10' "http://127.0.0.1:$PORT/baseline?n=5")"
expect "GET /baseline empty"  "0"    "$(curl -fsS "http://127.0.0.1:$PORT/baseline")"

JSON="$(curl -fsS "http://127.0.0.1:$PORT/json/3")"
[[ "$JSON" == *'"count":3'* && "$JSON" == *'"items"'* ]] \
    && pass "GET /json/3 shape" || fail "json shape" "$JSON"

JSON_BIG="$(curl -fsS "http://127.0.0.1:$PORT/json/9999")"
[[ "$JSON_BIG" == *'"count":64'* ]] \
    && pass "GET /json/9999 clamped to 64" || fail "json clamp" "$JSON_BIG"

STATUS="$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/missing")"
expect "404 unknown path" "404" "$STATUS"

CT="$(curl -sSI "http://127.0.0.1:$PORT/json/1" | awk -F': *' 'tolower($1)=="content-type"{print $2}' | tr -d '\r')"
expect "json content-type" "application/json" "$CT"

KA_SUM="$(curl -fsS -w '%{num_connects}\n' \
    -o /dev/null "http://127.0.0.1:$PORT/" \
    -o /dev/null "http://127.0.0.1:$PORT/" \
    -o /dev/null "http://127.0.0.1:$PORT/" \
    -o /dev/null "http://127.0.0.1:$PORT/" \
    -o /dev/null "http://127.0.0.1:$PORT/" \
    | awk '{s+=$1} END{print s}')"
expect "keep-alive (1 connect for 5 requests)" "1" "$KA_SUM"

if command -v nc >/dev/null; then
    PIPE="$(printf 'GET /pipeline HTTP/1.1\r\nHost: x\r\n\r\nGET /pipeline HTTP/1.1\r\nHost: x\r\n\r\nGET /pipeline HTTP/1.1\r\nHost: x\r\n\r\nGET /pipeline HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' \
        | timeout 3 nc -q1 127.0.0.1 "$PORT" 2>/dev/null | grep -c '^ok' || true)"
    [[ "$PIPE" -ge 4 ]] \
        && pass "HTTP/1.1 pipelining (4 responses)" \
        || fail "pipelining" "got $PIPE 'ok' bodies"
fi

DISTINCT="$(for _ in $(seq 1 64); do
    curl -fsS "http://127.0.0.1:$PORT/pid"
done | awk '{print $2}' | sort -u | wc -l)"
[[ "$DISTINCT" -ge 2 ]] \
    && pass "REUSEPORT spread across workers ($DISTINCT distinct counters)" \
    || fail "multi-worker" "only $DISTINCT distinct counter(s)"

if [[ "$TLS_AVAILABLE" == 1 ]]; then
    echo ">> TLS / HTTP/2 / HTTP/3 checks"

    H2="$(curl -sSk --http2 -w '%{http_code} %{http_version}' -o /dev/null "https://127.0.0.1:$TLS_PORT/")"
    expect "HTTP/2 ALPN handshake + 200" "200 2" "$H2"

    H2_BODY="$(curl -sSk --http2 "https://127.0.0.1:$TLS_PORT/json/2")"
    [[ "$H2_BODY" == *'"count":2'* ]] \
        && pass "HTTP/2 body roundtrip" || fail "h2 body" "$H2_BODY"

    ALTSVC="$(curl -sSkI --http2 "https://127.0.0.1:$TLS_PORT/" | awk -F': *' 'tolower($1)=="alt-svc"{print $2}' | tr -d '\r"')"
    [[ "$ALTSVC" == *"h3="* ]] \
        && pass "Alt-Svc advertises h3 ($ALTSVC)" \
        || fail "alt-svc" "got [$ALTSVC]"

    if [[ -x "$H3CLIENT" ]]; then
        H3_OUT="$("$H3CLIENT" 127.0.0.1 "$H3_PORT" / 2>&1)"
        if [[ "$H3_OUT" == *"STATUS=200"* && "$H3_OUT" == *"ok"* ]]; then
            pass "HTTP/3 GET / -> 200, body=ok"
        else
            fail "HTTP/3" "$H3_OUT"
        fi
        H3_JSON="$("$H3CLIENT" 127.0.0.1 "$H3_PORT" /json/3 2>&1)"
        [[ "$H3_JSON" == *'"count":3'* ]] \
            && pass "HTTP/3 GET /json/3" || fail "h3 json" "$H3_JSON"
    else
        echo "  SKIP  HTTP/3 (h3client not built)"
    fi
fi

if command -v wrk >/dev/null; then
    echo ">> 5s wrk -t4 -c64"
    wrk -t4 -c64 -d5s "http://127.0.0.1:$PORT/" | grep -E "Requests/sec|Latency|requests in" | sed 's/^/   /'
fi

echo ">> done"
