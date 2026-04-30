--TEST--
HttpServer: HTTP/3 single QUIC conn carries N sequential request streams
--EXTENSIONS--
true_async_server
true_async
--ENV--
PHP_HTTP3_DISABLE_RETRY=1
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* Regression for commit 741acb9 — h3client gained the
 * `H3CLIENT_REQUEST_COUNT=N` mode that reuses a single QUIC
 * connection for N sequential bidi request streams (the bench-mode
 * path). All existing H3 tests are single-shot; this one verifies
 * the reuse path actually works:
 *
 *   - 5 sequential requests over one QUIC conn
 *   - each opens a fresh bidi stream → h3_streams_opened == 5
 *   - server sees 5 distinct request_received events
 *   - response_submitted == 5
 *   - the QUIC connection is accepted exactly once (no per-request
 *     handshake — that's the whole point of reuse)
 *
 * H3CLIENT_QUIET=1 suppresses per-request STATUS/body dumps so the
 * output stays a one-line `COMPLETED=5`. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-117';
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
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);

    $cmd = sprintf('H3CLIENT_REQUEST_COUNT=5 H3CLIENT_QUIET=1 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $completed = -1;
    if (preg_match('/^COMPLETED=(\d+)$/m', $out, $m)) $completed = (int)$m[1];
    echo "completed=", $completed, "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "streams_opened=",     (int)($s['h3_streams_opened']     ?? -1), "\n";
    echo "request_received=",   (int)($s['h3_request_received']   ?? -1), "\n";
    echo "response_submitted=", (int)($s['h3_response_submitted'] ?? -1), "\n";
    /* quic_conn_accepted counts how many ngtcp2_conn_server_new calls
     * succeeded. Connection reuse means it stays at 1 — if it climbs
     * to 5, h3client is opening 5 separate connections instead of 5
     * streams on one. */
    echo "conn_accepted=",      (int)($s['quic_conn_accepted'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
completed=5
streams_opened=5
request_received=5
response_submitted=5
conn_accepted=1
done
