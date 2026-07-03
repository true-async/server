--TEST--
WebSocket: trySend() reports BUSY under outbound backpressure without blocking
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
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0)            // isolate: no server PING traffic
    ->setStreamWriteBufferBytes(4096);  // small high-water mark → backpressure

$server = new HttpServer($config);

// The client never reads frames, so the socket + batched tail fill quickly.
// trySend() must accept a few, then report BUSY (false) — and NEVER suspend,
// so the loop runs to completion.
$outcome = ['accepted' => 0, 'busy' => 0, 'completed' => false];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$outcome) {
    $payload = str_repeat('x', 2048);
    for ($i = 0; $i < 100; $i++) {
        if ($ws->trySend($payload)) {
            $outcome['accepted']++;
        } else {
            $outcome['busy']++;
        }
    }
    $outcome['completed'] = true;
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

$client = spawn(function () use ($port, $server) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);

    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    // Drain ONLY the handshake response; never read the data frames so the
    // server's outbound path stays backed up.
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    // Yield so the handler's (non-suspending) trySend loop runs to completion.
    delay(300);

    fclose($fp);
    $server->stop();
});

$server->start();
await($client);

echo "accepted>0: ", $outcome['accepted'] > 0 ? "yes" : "no", "\n";
echo "busy>0: ",     $outcome['busy']     > 0 ? "yes" : "no", "\n";
echo "completed: ",  $outcome['completed']     ? "yes" : "no", "\n";
echo "Done\n";
--EXPECT--
accepted>0: yes
busy>0: yes
completed: yes
Done
