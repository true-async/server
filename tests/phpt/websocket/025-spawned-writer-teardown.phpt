--TEST--
WebSocket: a spawned writer stuck in send() survives abrupt peer teardown (no UAF / double-commit)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/*
 * Regression for the spawned-writer teardown bug. A handler spawns a
 * dedicated writer coroutine that does $ws->send(BIG) to a client which
 * performs the handshake then stops reading, so the writer stalls inside
 * the transport send. The handler itself blocks in recv(). The client then
 * tears the connection down mid-send. Repeated across several connections.
 *
 * Two distinct defects this guards against:
 *   1. Double-commit: the handler's recv() and the writer's send() both
 *      reach ws_commit_upgrade before either finishes; a suspending 101
 *      send let the second swap the strategy again and free the first
 *      session under it (heap corruption).
 *   2. Shared-exception UAF: the peer-close read completion's exception was
 *      freed by the read callback while the writer's write-filter still
 *      forwarded it to resume the stalled writer.
 *
 * The worker must survive every iteration; a regression crashes the process
 * so "Done" never prints.
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19680 + getmypid() % 100;
$N    = 4;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(3)
    ->setWriteTimeout(1)          // a permanently stalled writer times out fast
    ->setWsPingIntervalMs(0);
$server = new HttpServer($config);

$big      = str_repeat('x', 4 * 1024 * 1024);   // exceeds loopback socket buffers
$handled  = 0;

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($big, &$handled) {
    // Dedicated writer — stalls because the client never reads.
    spawn(function () use ($ws, $big) {
        try { $ws->send($big); } catch (\Throwable $e) { /* peer went away */ }
    });
    // Handler stays alive until the peer closes.
    try { $ws->recv(); } catch (\Throwable $e) {}
    $handled++;
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server, $N) {
    for ($i = 0; $i < $N; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        if (!$fp) { delay(20); continue; }
        stream_set_timeout($fp, 2);
        fwrite($fp,
          "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

        // Read only the handshake; then stop reading so the writer stalls.
        $hs = '';
        $guard = 20;
        while (!str_contains($hs, "\r\n\r\n") && $guard-- > 0) {
            $c = @fread($fp, 4096);
            if ($c === false) break;
            $hs .= $c;
            if ($c === '') delay(5);
        }

        // Yank the socket while the server's writer is mid-send.
        delay(40);
        fclose($fp);
        delay(30);
    }
    delay(100);
    $server->stop();
});

$server->start();
await($client);

echo "handled>=1: ", ($handled >= 1 ? "yes" : "no"), "\n";
echo "Done\n";
--EXPECT--
handled>=1: yes
Done
