--TEST--
HttpServer: an HTTP/2 body refused past setMaxBodySize() wakes a coroutine parked in readBody() with the refusal
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* A streaming body has two kinds of waiter: awaitBody() parks on body_event,
 * readBody() on the chunk queue. The refusal fired body_event alone, so a
 * reader in a coroutine of its own stayed parked until the stream was torn
 * down and then saw a generic stream error instead of the 413. The handler
 * coroutine is cancelled by the reset either way, which is why the reader
 * here runs in a child. The request carries no Content-Length, so the body
 * streams from the headers on; the first DATA frame is drained before the
 * second one passes the cap, so the reader is parked on an empty queue when
 * the refusal lands. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBodyStreamingEnabled(true)
    ->setMaxBodySize(8 * 1024)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$seen = 'still parked';

$server->addHttpHandler(function ($req, $resp) use (&$seen) {
    await(spawn(function () use ($req, &$seen) {
        try {
            while ($req->readBody() !== null) {
            }

            $seen = 'end of body';
        } catch (\Throwable $e) {
            $seen = get_class($e) . ' ' . $e->getCode();
        }
    }));

    $resp->setStatusCode(200)->setBody('read');
});

$client = spawn(function () use ($port, $server, &$seen) {
    usleep(30000);
    $cli = new H2TestClient('127.0.0.1', $port, 5);
    $sid = $cli->sendRequest('POST', '/up', "127.0.0.1:$port", [], str_repeat('a', 1024), false);
    usleep(100000);
    $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('b', 16 * 1024));
    usleep(100000);
    echo 'reader: ', $seen, "\n";
    $cli->close();
    $server->stop();
});

$server->start();
await($client);
?>
--EXPECT--
reader: TrueAsync\HttpException 413
