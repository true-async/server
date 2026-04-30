--TEST--
HttpServer: HTTP/2 plaintext h2c prior-knowledge — GET round-trip
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
/* First end-to-end HTTP/2 round-trip.
 *
 * Client: curl --http2-prior-knowledge (no ALPN, plaintext h2c).
 * Server: detects the H2 connection preface on first read, spins up
 * http2_strategy, dispatches the request through the same handler the
 * HTTP/1 path uses, and routes the response via
 * http2_strategy_commit_response — submit_response + drain into the
 * socket. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19700 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'text/plain')
         ->setBody("hi from h2\n");
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    /* curl flags:
     *   --http2-prior-knowledge  — skip ALPN, speak H2 on plaintext.
     *   -sS                       — silent + show errors.
     *   -i                        — include response headers (for :status).
     *   --max-time 3              — guard against a server hang.
     *   -w "HTTP_VERSION:%{http_version}\n" — confirm we really spoke H2.
     */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -sS -i --max-time 3 '
        . '-w "HTTP_VERSION:%%{http_version}\n" '
        . 'http://127.0.0.1:%d/',
        $port
    );
    $out = [];
    $rc = 0;
    exec($cmd . ' 2>&1', $out, $rc);
    $body = implode("\n", $out);

    echo "curl_rc=$rc\n";
    echo "saw_200=",   (int) (strpos($body, '200') !== false), "\n";
    echo "saw_body=",  (int) (strpos($body, 'hi from h2') !== false), "\n";
    echo "saw_h2=",    (int) (strpos($body, 'HTTP_VERSION:2') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
curl_rc=0
saw_200=1
saw_body=1
saw_h2=1
Done
