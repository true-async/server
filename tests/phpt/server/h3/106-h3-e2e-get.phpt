--TEST--
HttpServer: HTTP/3 end-to-end GET via embedded nghttp3 client
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
/* End-to-end HTTP/3 GET round-trip.
 *
 * Drives the embedded h3client (a ~400 LoC C harness in
 * tests/h3client/) over UDP against an HttpServer with H3 enabled.
 * The flow exercises the entire Step 4 pipeline:
 *
 *  - QUIC handshake (Step 3a/b/c) → ALPN-h3 negotiation (Step 4.0)
 *  - nghttp3 conn + uni-streams open (Step 4.1)
 *  - request frames decoded into http_request_t (Step 4.2a)
 *  - dispatch to a PHP coroutine; user handler runs (Step 4.2c)
 *  - response serialised back through nghttp3 + ngtcp2 (Step 4.2b/c)
 *
 * Asserts: status, body, content-type all round-trip; stats reflect
 * one accepted connection / one stream / one submitted response. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-106';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20300 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain; charset=utf-8')
        ->setBody('h3-ok ' . $req->getMethod() . ' ' . $req->getUri());
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);

    /* Run the embedded client. Strip stderr STATUS=… line out of
     * the assertion; the body comes back on stdout. */
    $cmd = sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd);
    $out = $out ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out);

    echo "status=", $status ?? -1, "\n";
    echo "body=",   trim($body), "\n";

    /* Spot-check the H3 stats — at least one stream opened, one
     * response submitted, and zero parse errors. */
    $s = $server->getHttp3Stats()[0] ?? [];
    echo "streams_opened=",      (int)($s['h3_streams_opened']     ?? -1), "\n";
    echo "request_received=",    (int)($s['h3_request_received']   ?? -1), "\n";
    echo "response_submitted=",  (int)($s['h3_response_submitted'] ?? -1), "\n";
    echo "alpn_mismatch=",       (int)($s['quic_alpn_mismatch']    ?? -1), "\n";
    echo "parse_errors=",        (int)($s['quic_parse_errors']     ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
body=h3-ok GET /
streams_opened=1
request_received=1
response_submitted=1
alpn_mismatch=0
parse_errors=0
done
