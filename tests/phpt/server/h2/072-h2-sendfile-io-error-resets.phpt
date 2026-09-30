--TEST--
HttpServer: an HTTP/2 sendFile() body that ends in an I/O error resets its stream and keeps the connection
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* The fault point fails the first file read the way the file would, after the
 * HEADERS left and before any DATA did. The stream ends in
 * RST_STREAM(INTERNAL_ERROR = 2), not in an empty END_STREAM the peer would
 * take for a complete empty file, and the handler that called sendFile() is
 * not cancelled as if the peer had reset it: the next request on the same
 * connection is answered. The hit count proves the fault was injected. The
 * HTTP/1 counterpart is h1/066. */

require_once __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-072';
@mkdir($tmp, 0700, true);
$file = "$tmp/big.bin";
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', 2 * 1024 * 1024 / 16));

register_shutdown_function(function () use ($tmp, $file) {
    @unlink($file); @rmdir($tmp);
});

$point = 'h2/file_body/io_error';
$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use ($file) {
    $req->getPath() === '/f' ? $res->sendFile($file) : $res->setBody('small');
});

$client = spawn(function () use ($server, $port, $point) {
    usleep(100000);

    try {
        $cli = new H2TestClient('127.0.0.1', $port, 10);
        _http_fault_enable($point);

        $sid = $cli->sendRequest('GET', '/f', "127.0.0.1:$port");
        [$status, $body, , $ended] = $cli->collectResponse($sid, true);
        echo "failed: status=$status got=", strlen($body), " ended=", (int) $ended,
            " reset=", var_export($cli->lastResetCode(), true), "\n";
        echo "hits: ", _http_fault_hits($point), "\n";
        _http_fault_disable($point);

        $sid = $cli->sendRequest('GET', '/small', "127.0.0.1:$port");
        [$status, $body, , $ended] = $cli->collectResponse($sid, true);
        echo "next: status=$status body=$body ended=", (int) $ended, "\n";
        $cli->close();
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }

    $server->stop();
});

$watchdog = spawn(function () use ($server) {
    usleep(15000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$watchdog->cancel();
await($client);
?>
--EXPECT--
failed: status=200 got=0 ended=0 reset=2
hits: 1
next: status=200 body=small ended=1
