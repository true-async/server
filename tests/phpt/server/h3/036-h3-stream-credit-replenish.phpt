--TEST--
HttpServer: HTTP/3 replenishes bidi stream credit beyond initial_max_streams_bidi
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
/* Regression: a QUIC connection must keep serving requests past the
 * advertised initial_max_streams_bidi (default 100). ngtcp2 never
 * auto-extends MAX_STREAMS on stream close, so the server must call
 * ngtcp2_conn_extend_max_streams_bidi() in its stream_close callback.
 * Without that, every connection is permanently capped at 100 bidi
 * request streams — each one served fast, then the connection stalls
 * (this is what crushed the HttpArena baseline-h3 result to ~20 req/s
 * per connection).
 *
 * Drive 150 sequential request streams (> the 100 default) over a
 * single reused QUIC connection. Pre-fix: completed stalls at 100.
 * Post-fix: all 150 complete and the connection is accepted once. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-036';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20400 + (getmypid() % 40) + 16;

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

    $cmd = sprintf('H3CLIENT_REQUEST_COUNT=150 H3CLIENT_QUIET=1 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $completed = -1;
    if (preg_match('/^COMPLETED=(\d+)$/m', $out, $m)) $completed = (int)$m[1];
    echo "completed=", $completed, "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "streams_opened=",     (int)($s['h3_streams_opened']     ?? -1), "\n";
    echo "request_received=",   (int)($s['h3_request_received']   ?? -1), "\n";
    echo "response_submitted=", (int)($s['h3_response_submitted'] ?? -1), "\n";
    /* One connection, 150 streams — reuse must hold (no per-request
     * handshake) or the credit-replenish path isn't what's exercised. */
    echo "conn_accepted=",      (int)($s['quic_conn_accepted'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
completed=150
streams_opened=150
request_received=150
response_submitted=150
conn_accepted=1
done
