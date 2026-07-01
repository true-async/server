#!/usr/bin/env bash
#
# Full-docker Autobahn run: build the echo server image from scratch (PHP
# true-async fork + ext/async + true_async_server), bring it up, drive the
# wstest fuzzingclient against it on a shared docker network, grade the
# report. No host networking, no host PHP needed.
#
# Usage:
#   ./run-docker.sh            # smoke (RFC 6455 cases 1-7) — PR gate
#   ./run-docker.sh full       # all ~500 cases — nightly
#
# Env:
#   COMPOSE   docker compose invocation (default: "docker compose")
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
COMPOSE="${COMPOSE:-docker compose}"

MODE="${1:-smoke}"
case "$MODE" in
    smoke) export SPEC="fuzzingclient-docker-smoke.json" ;;
    full)  export SPEC="fuzzingclient-docker.json" ;;
    *) echo "usage: $0 [smoke|full]" >&2; exit 2 ;;
esac

# wstest writes reports as root (container uid 0); clean them via a
# throwaway root container so a non-root host user can re-run.
docker run --rm -v "$HERE:/spec" alpine rm -rf /spec/reports 2>/dev/null || true
cleanup() { $COMPOSE down -v --remove-orphans 2>/dev/null || true; }
trap cleanup EXIT

echo ">> building echo image + starting it (waits for healthcheck)"
# CI pre-builds the image (with layer caching) and sets NO_BUILD=1.
BUILD_FLAG="--build"
[ -n "${NO_BUILD:-}" ] && BUILD_FLAG=""
$COMPOSE up -d $BUILD_FLAG echo

echo ">> running Autobahn fuzzingclient ($MODE) -> $SPEC"
# autobahn's depends_on: service_healthy gates the start on the echo probe.
$COMPOSE run --rm autobahn wstest -m fuzzingclient -s "/spec/$SPEC"

echo ">> grading report"
# Grade with a throwaway PHP container so no host PHP is required.
docker run --rm -v "$HERE:/spec" -w /spec php:8.3-cli \
    php check_report.php reports/servers/index.json
