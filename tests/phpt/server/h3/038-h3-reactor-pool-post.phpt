--TEST--
HttpServer: HTTP/3 POST through the reactor/worker split — body crosses persistent (#80, D7.6)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Reactor-pool POST end-to-end (#80, D7.6 buffered).
 *
 * In the split, dispatch fires at headers-complete on the transport reactor,
 * but the body arrives afterwards — writing it into the request struct after
 * hand-off would race the worker and leave the body in the reactor's ZMM
 * (read cross-thread = corruption). The fix defers dispatch to stream-FIN in
 * reactor mode and materialises the body in the persistent (malloc) domain,
 * so the whole request crosses to the worker by pointer cleanly.
 *
 * The handler echoes len+sha1 of the received body; a corrupted or truncated
 * cross-thread body would surface as a hash mismatch (or a crash under ASan)
 * rather than a silent pass. 64 KiB spans multiple QUIC DATA frames. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-038';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$body_path = $tmp . '/body.bin';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

/* Deterministic 64 KiB so the expected hash is fixed. */
$body = str_repeat("0123456789abcdef", 4096);
file_put_contents($body_path, $body);
$expected = sprintf("len=%d sha1=%s method=POST", strlen($body), sha1($body));

register_shutdown_function(function () use ($tmp, $cert, $key, $body_path) {
    @unlink($cert); @unlink($key); @unlink($body_path); @rmdir($tmp);
});

$port = 21380 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $b = $req->getBody();
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody(sprintf("len=%d sha1=%s method=%s",
            strlen($b), sha1($b), $req->getMethod()));
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($port, $client_bin, $body_path, $expected) {
    usleep(600000);

    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d /upload POST %s 2>&1',
        escapeshellarg($client_bin), $port, escapeshellarg($body_path));
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = trim(preg_replace('/^STATUS=\d+\n?/m', '', $out));

    echo "status=", $status ?? -1, "\n";
    echo "match=", ($body === $expected ? "yes" : "no\n  got=$body\n  want=$expected"), "\n";

    /* Issue #11: no clean cross-thread pool shutdown yet. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Astatus=200
match=yes
%A
