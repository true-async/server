#!/usr/bin/env bash
# End-to-end proof that the observability example works:
#   server /metrics  ->  Prometheus scrape  ->  Grafana datasource
#
#   examples/docker/observability/smoke-test.sh
#
# Starts the PHP server on the host, brings the monitoring stack up, drives
# traffic, and asserts that each hop actually carries the numbers. Cleans up
# after itself.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

PHP="${PHP:-php}"
EXT="${EXT:-}"                      # e.g. EXT=-dextension=modules/true_async_server.so
PORT="${PORT:-8080}"
COMPOSE="docker compose -f $HERE/docker-compose.yml"

fail=0
check() {                            # check <label> <condition-result>
    if [ "$2" = "0" ]; then echo "  ok    $1"; else echo "  FAIL  $1"; fail=1; fi
}

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    $COMPOSE down -v >/dev/null 2>&1
}
trap cleanup EXIT

echo "==> starting the server on :$PORT"
PORT="$PORT" WORKERS=2 "$PHP" $EXT "$ROOT/examples/observability-server.php" >/tmp/tas-obs.log 2>&1 &
SRV_PID=$!

for _ in $(seq 1 30); do
    curl -sf "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.5
done

curl -sf "http://127.0.0.1:$PORT/" >/dev/null 2>&1
check "server answers" $?

curl -sf "http://127.0.0.1:$PORT/metrics" | grep -q '^# TYPE tas_requests_total counter'
check "/metrics exposes a Prometheus counter" $?

curl -sf "http://127.0.0.1:$PORT/metrics" | grep -q '^# TYPE tas_conns_active_h1 gauge'
check "/metrics exposes a Prometheus gauge" $?

curl -sf "http://127.0.0.1:$PORT/metrics" | grep -q 'tas_requests_total{worker="1"}'
check "/metrics splits series per worker" $?

echo "==> bringing up Prometheus + Grafana"
$COMPOSE up -d >/dev/null 2>&1
check "stack starts" $?

echo "==> driving traffic"
for _ in $(seq 1 20); do curl -sf "http://127.0.0.1:$PORT/" >/dev/null; done
for _ in $(seq 1 3);  do curl -sf "http://127.0.0.1:$PORT/boom" >/dev/null; done

echo "==> waiting for Prometheus to scrape"
scraped=1
for _ in $(seq 1 40); do
    # -G + --data-urlencode: the query carries braces, which must not hit the URL raw.
    if curl -sfG 'http://127.0.0.1:9090/api/v1/query' \
            --data-urlencode 'query=up{job="true-async-server"}' \
       | grep -q '"value":\[[^]]*,"1"\]'; then
        scraped=0
        break
    fi
    sleep 1
done
check "Prometheus scrapes the server (up == 1)" $scraped

got=1
for _ in $(seq 1 30); do
    v=$(curl -sf 'http://127.0.0.1:9090/api/v1/query?query=tas_requests_total' \
        | grep -o '"result":\[.' | head -1)
    if [ "$v" = '"result":[{' ]; then got=0; break; fi
    sleep 1
done
check "Prometheus stores tas_requests_total" $got

curl -sf 'http://127.0.0.1:9090/api/v1/query?query=tas_responses_5xx_total' \
    | grep -q '"__name__":"tas_responses_5xx_total"'
check "the 5xx we generated reached Prometheus" $?

echo "==> checking Grafana"
for _ in $(seq 1 40); do
    curl -sf http://127.0.0.1:3001/api/health >/dev/null 2>&1 && break
    sleep 1
done
curl -sf http://127.0.0.1:3001/api/health | grep -q '"database": *"ok"'
check "Grafana is healthy" $?

curl -sf http://127.0.0.1:3001/api/datasources | grep -q '"type":"prometheus"'
check "Grafana has the Prometheus datasource" $?

curl -sf 'http://127.0.0.1:3001/api/search?query=TrueAsync' | grep -q '"uid":"tas-overview"'
check "Grafana loaded the dashboard" $?

echo
if [ "$fail" = "0" ]; then echo "ALL OK"; else echo "FAILURES — see above"; fi
exit "$fail"
