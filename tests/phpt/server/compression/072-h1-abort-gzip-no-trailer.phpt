--TEST--
abort() under compression withholds the codec trailer as well as the terminator
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php if (!extension_loaded('zlib')) die('skip zlib required'); ?>
--FILE--
<?php
/* A closed deflate stream is a second claim that the body is whole, made by
 * the codec rather than by the framing, and a decoder checks it. Writing that
 * trailer over half a body would forge exactly the assurance an abort exists
 * to withhold — and it would be worse than the framing-level bug, because the
 * client would decode a short body without complaint.
 *
 * So the test asks the decoder. gzdecode() must refuse the bytes that arrived,
 * and the chunked terminator must be absent too: both finishers stay silent. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$state = [];

$server->addHttpHandler(function ($req, $res) use (&$state) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write(str_repeat('alpha', 200));

    $res->abort();

    $state['ended']    = $res->isEnded();
    $state['writable'] = $res->isWritable();

    /* Idempotent: the natural call site is a catch block. */
    $res->abort();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /s HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n");

    $raw = '';

    while (!feof($fp)) {
        $piece = fread($fp, 8192);

        if ($piece === false || $piece === '') {
            break;
        }

        $raw .= $piece;
    }

    $closed = feof($fp);
    fclose($fp);

    $head_end = strpos($raw, "\r\n\r\n");
    $body     = substr($raw, $head_end + 4);

    $gz = '';
    $terminated = false;
    $at = 0;

    while ($at < strlen($body)) {
        $nl = strpos($body, "\r\n", $at);

        if ($nl === false) {
            break;
        }

        $size = hexdec(substr($body, $at, $nl - $at));

        if ($size === 0) {
            $terminated = true;
            break;
        }

        $gz .= substr($body, $nl + 2, $size);
        $at = $nl + 2 + $size + 2;
    }

    echo "bytes arrived: ", (int) (strlen($gz) > 0), "\n";
    echo "terminator: ", (int) $terminated, "\n";
    echo "decoded: ", @gzdecode($gz) === false ? 'FAILED' : 'ok', "\n";
    echo "closed by server: ", (int) $closed, "\n";

    $server->stop();
});

$server->start();

echo "ended after abort: ", (int) $state['ended'], "\n";
echo "writable after abort: ", (int) $state['writable'], "\n";
?>
--EXPECT--
bytes arrived: 1
terminator: 0
decoded: FAILED
closed by server: 1
ended after abort: 1
writable after abort: 0
