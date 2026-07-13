--TEST--
getStats (#5): TLS/ALPN connections balance conns_active_h1 (no gauge underflow)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'curl' => true]);
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* An ALPN-negotiated TLS connection installs its strategy in the handshake
 * path, which bypasses the plaintext increment site; the close path decrements
 * unconditionally. Without a matching increment the uint64 gauge wraps to a
 * value near UINT64_MAX. After N HTTPS requests all closed, conns_active_h1
 * must be back at 0. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../tls/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-tls-gauge-' . getmypid();
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setStatsEnabled(true);

$server = new HttpServer($config);
$server->addHttpHandler(fn($req, $res) => $res->setStatusCode(200)->setBody('ok')->end());

$n = 4;

spawn(function () use ($port, $server, $cert, $n) {
    usleep(300000);

    for ($i = 0; $i < $n; $i++) {
        shell_exec(sprintf(
            'curl -s -o /dev/null --http1.1 --insecure --max-time 3 https://127.0.0.1:%d/%d',
            $port, $i));
    }

    $t = [];
    for ($p = 0; $p < 60; $p++) {
        $t = $server->getStats()['totals'];
        if (($t['total_requests'] ?? 0) >= $n && ($t['conns_active_h1'] ?? 1) === 0) {
            break;
        }
        usleep(20000);
    }

    echo 'requests=',   ($t['total_requests'] >= $n ? 1 : 0), "\n";
    /* The regression: an underflow shows a huge value, not 0. */
    echo 'h1_drained=', ($t['conns_active_h1'] === 0 ? 1 : 0), "\n";
    echo 'h1_sane=',    ($t['conns_active_h1'] < 1000 ? 1 : 0), "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
requests=1
h1_drained=1
h1_sane=1
done%A
