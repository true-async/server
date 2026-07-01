# WebSocket — remaining work

Tracks issue #2. The design and implementation are complete and covered by
`tests/phpt/websocket/` (30 tests), the Autobahn conformance runner
(`e2e/autobahn/`, 246/246 on `behavior`) and the frame-ingress fuzzer
(`fuzz/fuzz_ws_frame.c`). This file lists what is intentionally deferred —
it supersedes the original pre-implementation design plan.

## API ergonomics
- **`send()` / `sendBinary()` accept a `WebSocketMessage`** so echo/relay
  handlers can write `$ws->send($msg)` instead of branching on
  `$msg->binary`. (php_websocket.c + stub + arginfo.)
- **`IteratorAggregate`** — `foreach ($ws as $msg)` as an alternative to the
  `recv()` loop. Deferred.
- **`close(WebSocketCloseCode|int)`** — decide whether to drop the `int`
  half of the union (application codes 4000-4999) once the API settles.

## Behaviour
- **`WebSocketClosedException` propagation** — `recv()` currently returns
  null on every close; on a protocol / abnormal close it should throw
  `WebSocketClosedException` with `$closeCode` / `$closeReason` set.

## Telemetry & lifecycle
- Counters: active WS, frames in/out by opcode, control-frame ratio.
- Drain integration: send CLOSE 1001 on `max_connection_age_ms` / shutdown.

## Ops
- The Autobahn full run (`e2e/autobahn/`) is nightly; promote the smoke
  result to a required PR check once it has been stable for a few runs.
