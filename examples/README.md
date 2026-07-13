# Examples

Runnable, single-file demos for the `true_async_server` extension.

## Running

With a TrueAsync PHP build (the extension already loaded):

```bash
php examples/ws-echo-server.php
PORT=9000 php examples/ws-echo-server.php     # every example honours $PORT (default 8080)
```

If you built the extension locally and it is not in your `php.ini`:

```bash
php -d extension=true_async_server.so examples/ws-echo-server.php
```

Every server listens on `0.0.0.0:$PORT` and logs a start line to stderr. Stop it with Ctrl+C.

## WebSocket (RFC 6455)

| File | What it shows |
|---|---|
| [`ws-echo-server.php`](ws-echo-server.php) | Minimal echo — the `foreach ($ws as $msg)` pull loop, text vs binary. |
| [`ws-chat-server.php`](ws-chat-server.php) | Broadcast chat room — multi-producer `send()`, non-blocking `trySend()` for slow peers. |

Connect with [`websocat`](https://github.com/vi/websocat) (open two terminals for the chat demo):

```bash
websocat ws://127.0.0.1:8080/
```

…or from a browser console:

```js
const ws = new WebSocket('ws://127.0.0.1:8080/');
ws.onmessage = e => console.log('recv:', e.data);
ws.onopen    = () => ws.send('hello');
```

The same handlers work unchanged over `wss://` (TLS) and HTTP/2 Extended CONNECT (RFC 8441).

## gRPC (over HTTP/2 and HTTP/3)

| File | What it shows |
|---|---|
| [`grpc-greeter-server.php`](grpc-greeter-server.php) | Unary RPC — `readMessage()` in, `writeMessage()` out, `grpc-status` trailer. |
| [`grpc-stream-server.php`](grpc-stream-server.php) | Bidirectional streaming — echo each message; same shape covers server/client streaming. |

gRPC messages are your protobuf payloads; the 5-byte length-prefix framing is handled for
you. Call with a real client (grpcurl, generated stubs), or raw over h2c — a frame is
`1 flag byte + 4-byte big-endian length + payload`:

```bash
# unary greeter → "hello, world"
printf '\x00\x00\x00\x00\x05world' | curl --http2-prior-knowledge -s \
  -H 'content-type: application/grpc' -H 'te: trailers' --data-binary @- \
  http://127.0.0.1:8080/helloworld.Greeter/SayHello | tail -c +6; echo

# bidi echo, two stacked request frames → two reply frames
printf '\x00\x00\x00\x00\x03aaa\x00\x00\x00\x00\x02bb' | curl --http2-prior-knowledge -s \
  -H 'content-type: application/grpc' -H 'te: trailers' --data-binary @- \
  http://127.0.0.1:8080/echo.Echo/Stream | xxd
```

## Hot reload

| File | What it shows |
|---|---|
| [`reload-server.php`](reload-server.php) | Rotate the worker pool to pick up new code without dropping connections. |

Reload needs the worker pool (`setWorkers() > 1`). The bootloader is re-run on
replacement workers, so edits apply atomically per rotation:

```bash
php examples/reload-server.php          # prints the pool-parent pid
curl 127.0.0.1:8080/                    # version=v1 boot=<id>
echo v2 > examples/app-version.txt      # "change the app"
kill -HUP <pool-parent-pid>             # SIGHUP rotates the pool
curl 127.0.0.1:8080/                    # version=v2, fresh boot id, zero downtime
```

Triggers: `enableReloadOnSignal()` (SIGHUP, prod), `enableHotReload([dirs])`
(filesystem watcher, dev), or `$server->reload()` (programmatic).

## Observability (metrics + structured logs)

| File | What it shows |
|---|---|
| [`observability-server.php`](observability-server.php) | `getStats()` rendered as a Prometheus `/metrics` endpoint, plus a JSON access log in OpenTelemetry semantic conventions. |
| [`docker/observability/`](docker/observability/) | Prometheus + Grafana, with a pre-loaded dashboard. |

The server has no embedded exporter. `getStats()` returns a plain PHP array;
twenty lines in the example turn it into the Prometheus text format, and you can
swap them for OpenMetrics, StatsD or anything else without touching the server.

```bash
php examples/observability-server.php
curl -s localhost:8080/metrics

docker compose -f examples/docker/observability/docker-compose.yml up -d
open http://localhost:3001          # Grafana, no login, dashboard loaded

examples/docker/observability/smoke-test.sh   # asserts every hop end to end
```

The stack runs in Docker but the **server stays on the host** — it needs your
locally built extension, and the point is to scrape a real one.

**Why the counter kinds matter.** Run `reload()` while Grafana is open:
`tas_requests_total` keeps climbing (a retiring worker's totals are inherited,
so a scraper never sees a counter run backwards), while `tas_conns_active_h1`
drops to what is really open (a dead worker holds no connections, so its last
reading is not carried forward as a phantom).

**Two traps the example is built to avoid.** Under `setWorkers(N)` the handler is
copied into a worker thread, and that thread has its own function table — a
`function render()` declared in your script does not exist there, so anything the
handler calls must arrive through `use()` or a bootloader. And use the `file` log
sink, not `stream`: a parent-opened stream resource cannot cross into a worker.

## Other demos

| File | What it shows |
|---|---|
| [`minimal-server.php`](minimal-server.php) | Smallest possible handler — one line. |
| [`sse-server.php`](sse-server.php) | Server-Sent Events (`text/event-stream`) over the streaming pipeline. |
| [`demo-server.php`](demo-server.php) | A fuller HTTP demo with several routes. |
| [`multi-worker.php`](multi-worker.php) / [`multi-worker-manual.php`](multi-worker-manual.php) | Worker-pool setups. |
