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

## Other demos

| File | What it shows |
|---|---|
| [`minimal-server.php`](minimal-server.php) | Smallest possible handler — one line. |
| [`sse-server.php`](sse-server.php) | Server-Sent Events (`text/event-stream`) over the streaming pipeline. |
| [`demo-server.php`](demo-server.php) | A fuller HTTP demo with several routes. |
| [`multi-worker.php`](multi-worker.php) / [`multi-worker-manual.php`](multi-worker-manual.php) | Worker-pool setups. |
