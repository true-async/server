--TEST--
Compression H1 streaming: isWritable/tryWrite/awaitWritable answer from the transport, not the wrapper
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!function_exists('gzdecode')) die('skip zlib not built');
?>
--FILE--
<?php
/* A compressed streaming response runs behind a wrapper that owns an encoder
 * and no queue, so every backpressure question has to reach the transport
 * underneath it (src/compression/http_compression_response.c). A wrapper
 * answering for itself is what threw away an emitted deflate block in #177:
 * the caller was told to retry a chunk the encoder had already consumed.
 *
 * The handler asks all three on a live gzip stream. HTTP/1 keeps no queue, so
 * the answers are the ones an absent queue gives — true, accepted, ready —
 * and the point is that they come back at all and that the body still decodes
 * to the exact bytes afterwards. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$payload = str_repeat("compressible line for the backpressure probe\n", 200);
$probe   = [];

$server->addHttpHandler(function ($req, $res) use ($payload, &$probe, $server) {
    $res->setHeader('Content-Type', 'text/plain');

    $q = (int)(strlen($payload) / 4);

    $res->write(substr($payload, 0, $q));
    $probe['writable_after_first'] = $res->isWritable();

    $probe['try_accepted'] = $res->tryWrite(substr($payload, $q, $q));
    $probe['await_ready']  = $res->awaitWritable(1000);

    $res->write(substr($payload, 2 * $q, $q));
    $res->end(substr($payload, 3 * $q));

    $probe['ended']        = $res->isEnded();
    $probe['writable_end'] = $res->isWritable();

    $server->stop();
});

$cli = spawn(function () use ($port, $payload) {
    usleep(30000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n"
              . "Connection: close\r\n\r\n");

    $wire = '';
    while (!feof($fp)) {
        $c = fread($fp, 65536);
        if ($c === '' || $c === false) break;
        $wire .= $c;
    }
    fclose($fp);

    [$head, $rest] = explode("\r\n\r\n", $wire, 2);
    echo "gzip=", (int)(bool)preg_match('/^content-encoding:\s*gzip/mi', $head), "\n";
    echo "chunked=", (int)(bool)preg_match('/^transfer-encoding:\s*chunked/mi', $head), "\n";

    /* De-chunk, then inflate. */
    $body = '';
    $off  = 0;
    while (true) {
        $eol = strpos($rest, "\r\n", $off);
        if ($eol === false) break;
        $len = hexdec(substr($rest, $off, $eol - $off));
        $off = $eol + 2;
        if ($len === 0) break;
        $body .= substr($rest, $off, $len);
        $off  += $len + 2;
    }

    $plain = @gzdecode($body);
    echo "decoded=", (int)($plain === $payload), "\n";
    echo "smaller=", (int)(strlen($body) < strlen($payload)), "\n";
});

$server->start();
await($cli);

foreach ($probe as $k => $v) echo "$k = ", var_export($v, true), "\n";
echo "done\n";
?>
--EXPECT--
gzip=1
chunked=1
decoded=1
smaller=1
writable_after_first = true
try_accepted = true
await_ready = true
ended = true
writable_end = false
done
