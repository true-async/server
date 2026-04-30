--TEST--
WebSocket: H1 Upgrade handshake completes with 101 + Sec-WebSocket-Accept
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$port = 19850 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$handler_invoked = false;

// 2-arg WS handler. The recv loop is still throw-stubbed at this
// scaffold stage, so we just record the invocation and return.
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$handler_invoked) {
    $handler_invoked = true;
});

// HTTP handler is required because the server needs at least one to
// accept connections — the WS handler alone does not satisfy that
// preflight check today.
$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

$client = spawn(function () use ($port) {
    usleep(20000);
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        echo "client connect failed: $errstr\n";
        return null;
    }

    // RFC 6455 §1.3 sample request — Sec-WebSocket-Key chosen so the
    // expected Accept is the canonical fixture.
    $req = "GET /chat HTTP/1.1\r\n"
         . "Host: localhost\r\n"
         . "Upgrade: websocket\r\n"
         . "Connection: Upgrade\r\n"
         . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
         . "Sec-WebSocket-Version: 13\r\n"
         . "\r\n";
    fwrite($fp, $req);

    // Read until we have the full status + header block (terminated by \r\n\r\n).
    $resp = '';
    stream_set_timeout($fp, 2);
    while (($chunk = fread($fp, 4096)) !== '' && $chunk !== false) {
        $resp .= $chunk;
        if (str_contains($resp, "\r\n\r\n")) {
            break;
        }
    }
    fclose($fp);
    return $resp;
});

// Separate stopper coroutine — server->stop() called from a non-handler
// context is the supported pattern (see tests/phpt/server/h1/007).
$stopper = spawn(function () use ($server, $client) {
    await($client);
    // give the handler coroutine a tick to mark `$handler_invoked`
    usleep(20000);
    $server->stop();
});

$server->start();
$resp = await($client);
await($stopper);

if ($resp === null) {
    echo "no response\n";
    return;
}

// Verify the response shape — status line, mandatory headers, the
// canonical Accept value from RFC 6455 §1.3.
$lines = explode("\r\n", $resp);
echo "status: ", $lines[0], "\n";

$headers = [];
for ($i = 1; $i < count($lines) && $lines[$i] !== ''; $i++) {
    [$k, $v] = array_map('trim', explode(':', $lines[$i], 2));
    $headers[strtolower($k)] = $v;
}

echo "upgrade: ",    $headers['upgrade']    ?? '<none>', "\n";
echo "connection: ", $headers['connection'] ?? '<none>', "\n";
echo "accept: ",     $headers['sec-websocket-accept'] ?? '<none>', "\n";
echo "handler invoked: ", ($handler_invoked ? 'yes' : 'no'), "\n";
echo "Done\n";
--EXPECT--
status: HTTP/1.1 101 Switching Protocols
upgrade: websocket
connection: Upgrade
accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
handler invoked: yes
Done
