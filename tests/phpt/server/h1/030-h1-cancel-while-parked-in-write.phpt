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
 * Three assertions, and each one pins something different. The frames read
 * before the cancel are checked byte for byte, which is what catches a frame
 * assembled in the wrong order or with a piece already released. The tail read
 * after it must carry no terminator: sealing a frame the cancellation cut in
 * half is what desynchronises the next request on a kept-alive connection. And
 * the cancellation itself must arrive as HttpException 499 with isWritable()
 * false afterwards. A write timeout of one second turns a lost wake into a
 * bounded failure rather than a suite that hangs. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(30)->setWriteTimeout(1)
    ->setShutdownTimeout(0));          /* no grace window: cancel at once */

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    try {
        /* Far more than any socket buffer holds; parks after the first chunks. */
        for ($i = 0; $i < 20000; $i++) {
            $res->write(str_repeat('x', 2000));
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

    /* Take the opening frames, then stop reading so the handler parks. */
    delay(200);
    stream_set_blocking($fp, false);
    $head = '';

    for ($i = 0; $i < 40 && strlen($head) < 16384; $i++) {
        $c = fread($fp, 8192);
        if ($c !== false) $head .= $c;
        usleep(5000);
    }

    delay(500);
    $server->stop();
    delay(500);

    /* Whatever the cancelled response still had to say. */
    $tail = '';

    for ($i = 0; $i < 60; $i++) {
        $c = fread($fp, 65536);
        if ($c !== false) $tail .= $c;
        usleep(5000);
    }

    fclose($fp);

    [, $rest] = explode("\r\n\r\n", $head, 2);
    $sizes = [];
    $off   = 0;

    while (count($sizes) < 3) {
        $eol = strpos($rest, "\r\n", $off);
        if ($eol === false) break;
        $len = hexdec(substr($rest, $off, $eol - $off));
        $off = $eol + 2;
        if ($len === 0 || $off + $len + 2 > strlen($rest)) break;
        /* The frame must be exactly what the handler wrote, and end where the
         * size line said it would — a released or misplaced slot shows here. */
        if (substr($rest, $off, $len) !== str_repeat('x', $len)) break;
        if (substr($rest, $off + $len, 2) !== "\r\n") break;
        $sizes[] = $len;
        $off += $len + 2;
    }

    echo "frames=", implode(',', $sizes), "\n";
    echo "terminator_after_cancel=", (int)(strpos($tail, "0\r\n\r\n") !== false), "\n";
});

$server->start();
await($cli);

foreach ($seen as $k => $v) echo "$k = ", var_export($v, true), "\n";
echo "done\n";
?>
--EXPECT--
frames=2000,2000,2000
terminator_after_cancel=0
outcome = 'TrueAsync\\HttpException:499'
writable_after = false
done
