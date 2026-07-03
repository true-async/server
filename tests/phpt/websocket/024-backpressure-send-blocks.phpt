--TEST--
WebSocket: blocking send() suspends over the high-water mark and resumes once the queue drains
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
    ->setWsPingIntervalMs(0)
    ->setStreamWriteBufferBytes(4096);   // small high-water mark

$server = new HttpServer($config);

$outcome = ['busy_hit' => false, 'blocking' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$outcome) {
    // Fill the outbound queue past the high-water mark with non-blocking
    // sends (this tight loop never yields, so nothing drains meanwhile).
    $big = str_repeat('x', 2048);
    for ($i = 0; $i < 1000; $i++) {
        if (!$ws->trySend($big)) { $outcome['busy_hit'] = true; break; }
    }

    // A BLOCKING send over the high-water mark must suspend, then succeed once
    // the (reading) client drains the queue below the low-water mark — no
    // exception, no hang.
    try {
        $ws->send('SENTINEL');
        $outcome['blocking'] = 'ok';
    } catch (\Throwable $e) {
        $outcome['blocking'] = 'ex:' . $e::class;
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

$client = spawn(function () use ($port, $server) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 3);

    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    // Read continuously so the server's outbound queue drains and the blocking
    // send wakes. Stop once the sentinel frame arrives (proves the blocking
    // send completed after backpressure).
    stream_set_blocking($fp, false);
    $got = '';
    for ($i = 0; $i < 1000; $i++) {
        $c = fread($fp, 65536);
        if ($c !== false && $c !== '') {
            $got .= $c;
            if (str_contains($got, 'SENTINEL')) break;
        }
        delay(2);
    }

    fclose($fp);
    $server->stop();
    return str_contains($got, 'SENTINEL');
});

$server->start();
$sentinel_seen = await($client);

echo "busy_hit: ",      $outcome['busy_hit'] ? "yes" : "no", "\n";
echo "blocking: ",      $outcome['blocking'], "\n";
echo "sentinel_seen: ", $sentinel_seen ? "yes" : "no", "\n";
echo "Done\n";
--EXPECT--
busy_hit: yes
blocking: ok
sentinel_seen: yes
Done
