# Autobahn|Testsuite conformance runner

Drives the canonical [Autobahn|Testsuite](https://github.com/crossbario/autobahn-testsuite)
`fuzzingclient` (~500 RFC 6455 + RFC 7692 conformance cases) against the
TrueAsync WebSocket server — the ready-made, industry-standard protocol
conformance suite for RFC 6455.

## Layout

| File | Role |
|---|---|
| `server.php` | Echo server under test (text + binary echo, permessage-deflate on). |
| `fuzzingclient-smoke.json` | RFC 6455 core cases 1–7 — the **PR gate**. |
| `fuzzingclient.json` | All cases incl. compression (12–13) and limits (9–10) — **nightly**. |
| `run.sh` | Boots `server.php`, runs the `wstest` container, grades the report. |
| `check_report.php` | Fails CI if any case verdict is worse than `INFORMATIONAL`. |

## Running

Requires Docker (pulls `crossbario/autobahn-testsuite`) and a built
extension in `../../modules`.

```bash
# RFC 6455 core (cases 1-7) — fast, gates each PR
./run.sh

# Full ~500-case run — nightly
./run.sh full
```

Override the defaults via env:

```bash
PHP=/home/edmond/php-release/bin/php EXT_DIR=../../modules WS_PORT=9001 ./run.sh full
```

`ext/async` is assumed to be compiled into the PHP binary (this repo's dev
build). On a build where it ships as a shared module, point `TRUE_ASYNC` at
it:

```bash
TRUE_ASYNC=true_async ./run.sh          # by extension name (in extension_dir)
TRUE_ASYNC=/path/to/true_async.so ./run.sh   # by absolute path
```

The HTML/JSON report lands in `reports/servers/` (git-ignored). Open
`reports/servers/index.html` for the per-case breakdown.

## Verdict grading

`check_report.php` treats `OK`, `NON-STRICT`, and `INFORMATIONAL` as
passing; `FAILED`, `WRONG CODE`, `UNCLEAN`, and `UNIMPLEMENTED` fail the
run (non-zero exit). Both the frame-level `behavior` and the
close-handshake `behaviorClose` must pass.

## Networking note

`run.sh` launches `wstest` with `--network host`, so the container reaches
the echo server at `ws://127.0.0.1:9001` directly. On Docker Desktop
(macOS/Windows, where `--network host` is limited) change the `url` in the
spec files to `ws://host.docker.internal:9001` and drop `--network host`.
