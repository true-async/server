--TEST--
HttpServerConfig::addHttp2Listener — port refuses HTTP/1.1 even when an HTTP/1 handler is registered
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
/* Per-listener protocol mask. The server has BOTH addHttpHandler (H1+H2)
 * AND addHttp2Handler registered, so the server-wide mask still allows
 * HTTP/1.1. But the h2-only listener narrows that to H2 only — a plain
 * HTTP/1 GET on the h2c port must be rejected via nghttp2 BAD_CLIENT_MAGIC,
 * while the dual listener on a different port still serves H1 normally. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$pid = getmypid();
$port_dual = 19980 + $pid % 50;
$port_h2c  = 20030 + $pid % 50;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port_dual)
        ->addHttp2Listener('127.0.0.1', $port_h2c)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('hello');
});
$server->addHttp2Handler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('h2');
});

$client = spawn(function () use ($port_dual, $port_h2c, $server) {
    usleep(30000);
    $devnull = PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';

    /* Dual listener — plain HTTP/1 GET works. */
    $cmd = sprintf(
        'curl --http1.1 -s -o %s -w "%%{http_code}" --max-time 2 http://127.0.0.1:%d/',
        $devnull, $port_dual);
    exec($cmd, $out, $rc);
    echo "dual_h1 rc=$rc code=", implode('', $out), "\n";
    $out = [];

    /* h2-only listener — h2 prior knowledge works. */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -o %s -w "%%{http_code}" --max-time 2 http://127.0.0.1:%d/',
        $devnull, $port_h2c);
    exec($cmd, $out, $rc);
    echo "h2only_h2 rc=$rc code=", implode('', $out), "\n";
    $out = [];

    /* h2-only listener — plain HTTP/1 GET must be rejected even though
     * an H1 handler is registered: the listener mask wins. */
    $cmd = sprintf(
        'curl --http1.1 -s -o %s -w "%%{http_code}" --max-time 2 http://127.0.0.1:%d/',
        $devnull, $port_h2c);
    exec($cmd, $out, $rc);
    echo "h2only_h1 rejected=", (int)($rc !== 0 || (int)implode('', $out) === 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
dual_h1 rc=0 code=200
h2only_h2 rc=0 code=200
h2only_h1 rejected=1
done
