--TEST--
HttpServer: HTTP/3 response with >32 headers exercises submit_response heap-overflow path
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
/* Targeted regression for commit 1c371be — `submit_response` switched
 * from two-pass count+emit to single-pass with stack scratch[32] →
 * heap promotion (cap 64) → geometric doubling. Until this test
 * existed, no phpt sent more than a handful of response headers, so
 * the heap-promotion + first realloc-doubling branches were untested.
 *
 * Handler emits 70 distinct custom headers — :status + 70 = 71 nv
 * entries. Order of operations in src/http3/http3_connection.c:
 *
 *   nvi = 1   (just :status)
 *   nvi = 32  (scratch full)            → next entry triggers promotion
 *   nvi = 64  (heap full at first cap)  → next entry triggers double to 128
 *   nvi = 71  (final)                   → submit
 *
 * Both new branches are exercised. We ask h3client to emit HEADERS=N
 * via H3CLIENT_VERBOSE_HEADERS=1 and assert that exactly 70 non-status
 * response headers come back over the wire. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-116';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20300 + (getmypid() % 40) + 8;   /* keep clear of 106's range */

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain');
    /* 70 distinct, lowercase, RFC 9114-legal header names. Avoid the
     * forbidden set (connection / keep-alive / transfer-encoding /
     * upgrade / content-length) that h3_response_header_allowed
     * filters out — those would skew the count we assert. */
    for ($i = 0; $i < 70; $i++) {
        $res->setHeader(sprintf('x-overflow-%02d', $i), 'v' . $i);
    }
    $res->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);

    /* H3CLIENT_VERBOSE_HEADERS=1 makes h3client emit `HEADERS=N` to
     * stderr right after STATUS=. */
    $cmd = sprintf('H3CLIENT_VERBOSE_HEADERS=1 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = null; $headers = null;
    if (preg_match('/^STATUS=(\d+)$/m',  $out, $m)) $status  = (int)$m[1];
    if (preg_match('/^HEADERS=(\d+)$/m', $out, $m)) $headers = (int)$m[1];

    /* Strip both meta lines from body. */
    $body = preg_replace('/^(STATUS|HEADERS)=\d+\n?/m', '', $out);

    echo "status=",       $status  ?? -1, "\n";
    echo "header_count=", $headers ?? -1, "\n";
    /* content-type is set above plus 70 x-overflow-* — peer should see
     * exactly 71 non-status headers. content-type counts; the forbidden
     * content-length is filtered out by h3_response_header_allowed and
     * never reaches the wire. */
    echo "expected=71\n";
    echo "body=", trim($body), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
header_count=71
expected=71
body=ok
done
