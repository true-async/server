--TEST--
HttpServer: addHttp2Handler without addHttpHandler = h2-only listener
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
/* Protocol-registry Phase 1: a server that registers ONLY
 * addHttp2Handler opts into h2-only mode. The detector rejects
 * non-h2 garbage (no fallback to HTTP/1) and routes malformed
 * preface through the h2 bad-preface GOAWAY path.
 *
 * Verifies:
 *  - valid h2 request still works (handler runs, status round-trips);
 *  - plain HTTP/1 GET is rejected — the h2 strategy doesn't parse
 *    "GET / HTTP/1.1" as a valid connection preface, bad_preface
 *    GOAWAY is emitted, curl exits with a non-zero failure code. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19880 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

/* ONLY the h2 handler — no addHttpHandler call. */
$server->addHttp2Handler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('h2-only');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $devnull = PHP_OS_FAMILY === 'Windows' ? 'nul' : '/dev/null';

    /* Happy path: curl --http2-prior-knowledge succeeds. */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -o %s -w "%%{http_code}" '
        . '--max-time 2 http://127.0.0.1:%d/', $devnull, $port);
    exec($cmd, $out, $rc);
    echo "h2 rc=$rc code=", implode('', $out), "\n";
    $out = [];

    /* Negative: plain HTTP/1 GET. The detector sees "GET / HTTP/1.1"
     * which does NOT start with "PRI " and HTTP/1 is not in the mask,
     * so classification is HTTP_PROTOCOL_UNSUPPORTED → routed to h2
     * strategy → nghttp2 bails with BAD_CLIENT_MAGIC → hand-crafted
     * GOAWAY(PROTOCOL_ERROR) + close. curl (which expected a valid
     * HTTP/1 response) sees the binary frame and exits non-zero. */
    $cmd2 = sprintf(
        'curl --http1.1 -s -o %s -w "%%{http_code}" '
        . '--max-time 2 http://127.0.0.1:%d/', $devnull, $port);
    exec($cmd2, $out2, $rc2);
    echo "h1 rejected=", (int)($rc2 !== 0 || (int)implode('', $out2) === 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
h2 rc=0 code=200
h1 rejected=1
done
