--TEST--
HttpResponse::write() — chunk framing is identical either side of the coalescing threshold
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A chunk at or below H1_CHUNK_COALESCE_MAX leaves as one write, a larger one
 * as three (src/http1/http1_stream.c). The split is an optimisation and must
 * not show on the wire: this reads the raw response and checks the chunk-size
 * lines against the lengths the handler wrote, on both sides of the 32 KiB
 * boundary, plus the de-chunked body byte for byte. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port  = tas_free_port();
$sizes = [1024, 32 * 1024, 32 * 1024 + 1, 64 * 1024];

$expected = '';
foreach ($sizes as $i => $n) {
    $expected .= str_repeat(chr(65 + $i), $n);
}

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10));

$server->addHttpHandler(function ($req, $res) use ($sizes, $server) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    foreach ($sizes as $i => $n) {
        $res->write(str_repeat(chr(65 + $i), $n));
    }

    $res->end();
    $server->stop();
});

$cli = spawn(function () use ($port, $sizes, $expected) {
    usleep(30000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    $wire = '';
    while (!feof($fp)) {
        $c = fread($fp, 65536);
        if ($c === '' || $c === false) break;
        $wire .= $c;
    }
    fclose($fp);

    [$head, $rest] = explode("\r\n\r\n", $wire, 2);
    echo "chunked=", (int)(bool)preg_match('/^transfer-encoding:\s*chunked/mi', $head), "\n";

    /* Walk the chunked body by hand: every size line must match what the
     * handler wrote, in order, and the terminator must be the last thing. */
    $body  = '';
    $seen  = [];
    $off   = 0;
    while (true) {
        $eol = strpos($rest, "\r\n", $off);
        if ($eol === false) { echo "TRUNCATED\n"; break; }
        $len = hexdec(substr($rest, $off, $eol - $off));
        $off = $eol + 2;
        if ($len === 0) break;
        $seen[] = $len;
        $body  .= substr($rest, $off, $len);
        $off   += $len + 2;
    }

    echo "sizes_match=", (int)($seen === $sizes), "\n";
    echo "sizes=", implode(',', $seen), "\n";
    echo "body_len=", strlen($body), "\n";
    echo "body_match=", (int)($body === $expected), "\n";
});

$server->start();
await($cli);
echo "done\n";
?>
--EXPECT--
chunked=1
sizes_match=1
sizes=1024,32768,32769,65536
body_len=132097
body_match=1
done
