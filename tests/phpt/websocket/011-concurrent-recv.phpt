--TEST--
WebSocket: a second concurrent recv() throws WebSocketConcurrentReadException
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketConcurrentReadException;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19910 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$reader_outcome = ['caught' => false, 'class' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$reader_outcome) {
    // Spawn the spy FIRST with a tiny async delay, so main has time
    // to enter the long-suspending recv() and stake out the waiter
    // slot. When the delay completes the spy attempts its own recv()
    // and must hit the single-reader guard.
    $spy = spawn(function () use ($ws, &$reader_outcome) {
        delay(50);   // 50ms — long enough for main to suspend
        try {
            $ws->recv();
        } catch (\Throwable $e) {
            $reader_outcome['caught'] = true;
            $reader_outcome['class']  = $e::class;
        }
    });

    // Main blocks here on no incoming frames; peer-close eventually
    // wakes us with null.
    $ws->recv();
    await($spy);
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }
    // Don't send anything — let the FIRST recv() suspend and let
    // the spawned spy hit the concurrent-recv guard.
    usleep(80000);
    fclose($fp);
    usleep(50000);
    $server->stop();
});

$server->start();
await($client);

var_dump($reader_outcome['caught']);
echo "class: ", $reader_outcome['class'] ?? '<none>', "\n";
echo "Done\n";
--EXPECT--
bool(true)
class: TrueAsync\WebSocketConcurrentReadException
Done
