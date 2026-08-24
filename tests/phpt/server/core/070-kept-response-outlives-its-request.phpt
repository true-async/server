--TEST--
A $response kept past its request answers instead of reading the freed transport ctx (#256)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The response object carries the transport's per-request context behind
 * stream_ops. That context is freed when the request finalizes, so a handler
 * that stashes $response somewhere longer-lived — a global, a queue, a cache —
 * left the pair pointing at free memory: the churn below recycles it, and
 * isWritable() then read a foreign allocation. The pair is cleared with the
 * context now, which is the answer the API already has for a response with no
 * transport: not writable. */

require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

$port = tas_free_port();
$kept = null;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);

$server->addHttpHandler(function ($req, $res) use (&$kept) {
    if ($req->getPath() === '/keep') {
        $kept = $res;
        $res->setStatusCode(200)->setBody("kept\n");
        return;
    }

    if ($req->getPath() === '/touch') {
        $alive = $kept === null ? 'none' : var_export($kept->isWritable(), true);
        $res->setStatusCode(200)->setBody("alive={$alive}\n");
        return;
    }

    $res->setStatusCode(200)->setBody("ok\n");
});

spawn(function () use ($port, $server) {
    delay(200);

    $get = static function (string $path) use ($port) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);

        if ($fp === false) {
            return '';
        }

        stream_set_timeout($fp, 3);
        fwrite($fp, "GET {$path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
        $wire = stream_get_contents($fp);
        fclose($fp);

        return $wire;
    };

    $body = static function (string $wire): string {
        $parts = explode("\r\n\r\n", $wire, 2);

        return trim($parts[1] ?? '');
    };

    echo $body($get('/keep')), "\n";

    /* Recycle the freed per-request context: without the fix the read below
     * lands in whatever took its place. */
    for ($i = 0; $i < 200; $i++) {
        $get('/churn');
    }

    echo $body($get('/touch')), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
kept
alive=false
