--TEST--
Compression — a declared Content-Length keeps the response identity
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
?>
--FILE--
<?php
/* A codec puts a different number of bytes on the wire than the handler wrote,
 * which is why the encoder deletes any Content-Length it finds. Between the
 * two the declaration wins: the handler asked for a body of a known size, and
 * a client reading that length off the header must find that many bytes.
 *
 * Same payload, same Accept-Encoding, two routes. The undeclared one is gzip,
 * proving the negotiation would have fired; the declared one is identity, and
 * its body arrives byte for byte at the declared length. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$payload = str_repeat("compressible payload\n", 200);

$server->addHttpHandler(function ($req, $res) use ($payload) {
    $res->setHeader('Content-Type', 'text/html');

    if ($req->getPath() === '/declared') {
        $res->setHeader('Content-Length', (string) strlen($payload));
    }

    $res->write(substr($payload, 0, 100));
    $res->end(substr($payload, 100));
});

spawn(function () use ($port, $server, $payload) {
    usleep(50000);

    $fetch = static function (string $path) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\n"
                  . "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n");
        $raw = '';

        while (!feof($fp)) {
            $c = fread($fp, 8192);

            if ($c === '' || $c === false) {
                break;
            }

            $raw .= $c;
        }

        fclose($fp);
        [$head, $rest] = explode("\r\n\r\n", $raw, 2) + ['', ''];

        return [$head, $rest];
    };

    [$head, $body] = $fetch('/plain');
    echo "plain encoding: ", preg_match('/^content-encoding:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "plain chunked: ", (int) (bool) preg_match('/^transfer-encoding:\s*chunked/mi', $head), "\n";

    [$head, $body] = $fetch('/declared');
    echo "declared encoding: ", preg_match('/^content-encoding:\s*(\S+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "declared chunked: ", (int) (bool) preg_match('/^transfer-encoding:\s*chunked/mi', $head), "\n";
    echo "declared length: ", preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : '<absent>', "\n";
    echo "arrived: ", strlen($body), "\n";
    echo "identical: ", (int) ($body === $payload), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
plain encoding: gzip
plain chunked: 1
declared encoding: <absent>
declared chunked: 0
declared length: 4200
arrived: 4200
identical: 1
