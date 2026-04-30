--TEST--
HttpServer: HTTP/3 oversized request headers reject the stream, not the connection
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* Regression for the audit P3 #6 fix: when a peer's request headers
 * exceed HTTP3_MAX_HEADERS_BYTES (256 KiB), the server now responds
 * with a stream-level RFC 9114 H3_REQUEST_REJECTED — STOP_SENDING +
 * RESET_STREAM on that one stream — instead of returning
 * NGHTTP3_ERR_CALLBACK_FAILURE which used to kill the whole QUIC
 * connection.
 *
 * Test: drive 2 sequential requests on a single QUIC connection.
 * - Request #1 carries a 300-KiB x-bloat header → trips the cap.
 * - Request #2 is normal.
 *
 * Expected: #1 surfaces as a stream close with no status (server
 * never dispatched the handler), #2 returns 200 — proving the
 * connection survived. Server stats show h3_request_oversized==1
 * and the handler ran exactly once. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

/* Lift the per-stream QUIC FC ceiling from 256 KiB to 16 MiB so a
 * 300-KiB header block actually arrives at the server. The
 * HTTP3_MAX_HEADERS_BYTES cap (256 KiB) we're testing is independent
 * of QUIC flow control. */
putenv('PHP_HTTP3_BENCH_FC=1');

$tmp = __DIR__ . '/tmp-118';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20300 + (getmypid() % 40) + 16;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);

$dispatched = 0;
$server->addHttpHandler(function ($req, $res) use (&$dispatched) {
    $dispatched++;
    $res->setStatusCode(200)->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, &$dispatched) {
    usleep(80000);

    /* H3CLIENT_BLOAT_HEADER_KIB=300 → first request gets a 300-KiB
     * x-bloat header (above the 256-KiB server cap). Second request is
     * normal. H3CLIENT_REQUEST_COUNT=2 reuses the connection. */
    $cmd = sprintf(
        'H3CLIENT_REQUEST_COUNT=2 H3CLIENT_BLOAT_HEADER_KIB=300 '
        . '%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    /* h3client emits one STATUS=N line per completed request. Two
     * lines = the connection survived to carry the second request,
     * which is the whole point of the stream-level reject. */
    preg_match_all('/^STATUS=(\d+)$/m', $out, $m);
    $statuses = array_map('intval', $m[1]);

    /* Pull the H3 listener stats. h3_request_oversized must have
     * tripped exactly once (the bloat request). The handler must
     * have been dispatched exactly once (the normal request). */
    $stats = $server->getHttp3Stats()[0] ?? [];

    echo "status_count=", count($statuses), "\n";
    echo "first_status=",  $statuses[0] ?? -1, "\n";
    echo "second_status=", $statuses[1] ?? -1, "\n";
    echo "oversized=", (int)($stats['h3_request_oversized'] ?? -1), "\n";
    echo "dispatched=", $dispatched, "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status_count=2
first_status=0
second_status=200
oversized=1
dispatched=1
done
