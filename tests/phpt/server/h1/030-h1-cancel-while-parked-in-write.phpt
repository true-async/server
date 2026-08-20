--TEST--
HttpResponse::write() — a handler cancelled while parked inside a write unwinds cleanly
--EXTENSIONS--
true_async_server
true_async
sockets
--FILE--
<?php
/* The shape nothing in this suite covered, and the reason a lifetime defect on
 * this path stayed green for months: a handler parked inside a write, then
 * cancelled while the write is still in libuv's queue.
 *
 * Getting there needs three things at once. The peer must exist and must not
 * read, so the write parks instead of failing — SO_RCVBUF is shrunk to 4 KiB so
 * the server's socket buffer fills within one chunk. The cancellation must
 * arrive while the handler is parked, which means skipping the grace window:
 * stop() waits shutdown_timeout_s for handlers to finish on their own and only
 * then cancels the scope, so at the default of five seconds the handler always
 * wins the race. And the response must be streaming, because that is the only
 * path where the handler owns the write.
 *
 * What it asserts is the contract, not the defect: the cancellation reaches the
 * handler as HttpException 499, start() returns, and the process ends. A hang
 * here means the cancel never arrived; a crash means the frame outlived
 * something it pointed at. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(30)->setWriteTimeout(30)
    ->setShutdownTimeout(0));          /* no grace window: cancel at once */

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    try {
        /* Far more than any socket buffer holds; parks after the first chunks. */
        for ($i = 0; $i < 5000; $i++) {
            $res->write(str_repeat('x', 20000));
        }
        $res->end();
        $seen['outcome'] = 'finished';
    } catch (\Throwable $e) {
        $seen['outcome'] = get_class($e) . ':' . $e->getCode();
        $seen['writable_after'] = $res->isWritable();
    }
});

$cli = spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    $sock = socket_import_stream($fp);
    socket_set_option($sock, SOL_SOCKET, SO_RCVBUF, 4096);

    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    /* Read nothing at all: the handler parks inside the write. */
    delay(500);
    $server->stop();
    delay(500);
    fclose($fp);
});

$server->start();
await($cli);

foreach ($seen as $k => $v) echo "$k = ", var_export($v, true), "\n";
echo "done\n";
?>
--EXPECT--
outcome = 'TrueAsync\\HttpException:499'
writable_after = false
done
