--TEST--
HttpServer: HTTP/2 plaintext h2c — POST body round-trip
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
/* POST a non-trivial body over h2c and verify byte-exact echo.
 * Exercises on_data_chunk_recv_cb + on_frame_recv END_STREAM body
 * finalization (PLAN_HTTP2 Step 5a) — dispatch fires at END_HEADERS,
 * the coroutine is enqueued, body arrives via DATA frame(s), handler
 * runs after mem_recv returns with body already sealed. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19710 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    /* Echo the body back with a length marker so the test can verify
     * both presence and byte-exactness. */
    $body = $req->getBody();
    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'text/plain')
         ->setBody(sprintf("len=%d\n%s", strlen($body), $body));
});

$client = spawn(function() use ($port, $server) {
    usleep(30000);

    /* Deterministic 12-KiB body — crosses the single-chunk path
     * and forces at least one DATA frame split if MAX_FRAME_SIZE
     * shows up on the client side (which for curl is typically
     * 16 KiB, so 12 K fits in one frame — still exercises the
     * on_data_chunk_recv path). */
    $body = str_repeat("abcdefghij", 1228);   /* 12 280 bytes */
    $tmp  = tempnam(sys_get_temp_dir(), 'h2body_');
    file_put_contents($tmp, $body);

    $cmd = sprintf(
        'curl --http2-prior-knowledge -sS --max-time 5 '
        . '-X POST --data-binary @%s http://127.0.0.1:%d/echo',
        escapeshellarg($tmp), $port
    );
    $out = [];
    $rc  = 0;
    exec($cmd . ' 2>&1', $out, $rc);
    $resp = implode("\n", $out);
    @unlink($tmp);

    /* Response body should be "len=<N>\n<body>". Verify N + hash match. */
    $expected_prefix = "len=" . strlen($body);
    $got_prefix  = strtok($resp, "\n");
    $got_body    = substr($resp, strlen($got_prefix) + 1);

    echo "curl_rc=$rc\n";
    echo "prefix_ok=", (int)($got_prefix === $expected_prefix), "\n";
    echo "body_ok=",   (int)($got_body === $body), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
curl_rc=0
prefix_ok=1
body_ok=1
Done
