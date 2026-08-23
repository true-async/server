--TEST--
HTTP/3 — a handler killed by a bailout answers 500, not its half-built body
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--INI--
memory_limit=16M
display_errors=0
log_errors=0
--FILE--
<?php
/* The handler writes a status and part of a body, then exhausts memory_limit —
 * an E_ERROR, and therefore a zend_bailout. The dispose read the bailout flag
 * for telemetry and for the streaming abort, but the buffered commit did not:
 * it submitted the response object as the handler had left it, so the peer read
 * a complete 200 over a body that stops where the handler died.
 *
 * The probe is aioquic, not h3client, so the answer is read by a QUIC stack the
 * server does not share. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = sys_get_temp_dir() . '/h3-bailout-' . getmypid();
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";

if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($dir, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($dir);
});

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addHttp3Listener('127.0.0.1', $port)
        ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
        ->setReadTimeout(10)->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('half-built');
    $doomed = str_repeat('a', 64 * 1024 * 1024);
    $res->setBody('unreachable');
});

spawn(function () use ($port, $server) {
    delay(400);

    $probe = __DIR__ . '/../../../h3client/h3probe.py';
    $line  = trim((string) shell_exec(sprintf(
        'python3 %s 127.0.0.1 %d /x 2>/dev/null',
        escapeshellarg($probe), $port)));

    delay(200);
    $server->stop();

    /* Printed after the last request: the firewall dumps a C backtrace to
     * stderr per bailout, and run-tests compares stderr along with stdout. */
    echo $line, "\n";
});

$server->start();
?>
--EXPECTF--
%Astatus=500 bytes=21 cl=21 outcome=CLEAN_END
