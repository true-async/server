--TEST--
HttpServer: HTTP/2 handler that throws yields 500 and does NOT abort the worker
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
/* Regression for #101: an uncaught exception from an HTTP/2 handler used
 * to leak the exception into the scheduler's EG(exception), trip a
 * premature graceful shutdown, and abort the worker at teardown
 * (ZEND_ASYNC_REACTOR_LOOP_ALIVE assertion). The handler dispose now marks
 * the exception consumed on both escalation paths (finalize + object
 * destroy), so the peer gets a clean HTTP/2 500 and the process exits 0. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    throw new \RuntimeException('boom');   // uncaught
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -sS -o /dev/null --max-time 3 '
        . '-w "%%{http_code}" http://127.0.0.1:%d/',
        $port
    );
    $out = [];
    $rc = 0;
    exec($cmd . ' 2>&1', $out, $rc);

    echo "curl_rc=$rc\n";
    echo "http_code=", trim(implode('', $out)), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "reached end\n";
--EXPECT--
curl_rc=0
http_code=500
reached end
