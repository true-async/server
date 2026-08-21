--TEST--
HttpServer: a failed stream on a pool worker resets the reactor's stream (#171, gated pool)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Under the pool the handler runs on a worker thread that owns no QUIC stream:
 * everything it produces travels to the reactor as a wire message, and the
 * reactor is the only side that can reset anything. The abort therefore has to
 * survive that crossing — as a terminal wire of a different kind, not as the
 * clean one with a flag the reactor might ignore.
 *
 * Same assertion as 058 on the in-thread path, so the two can be compared: the
 * peer must see RESET(err=258) rather than a short body that ended. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-059';
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($dir, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($dir);
});

$probe = __DIR__ . '/../../../h3client/h3probe.py';
$port  = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain; charset=utf-8');
    $res->write('chunk1;');

    throw new \RuntimeException('handler failed mid-body');
});

spawn(function () use ($server, $port, $probe) {
    usleep(600000);

    $cmd = sprintf('python3 %s 127.0.0.1 %d /stream 2>/dev/null',
        escapeshellarg($probe), $port);
    $out = (string) shell_exec($cmd);

    $status  = preg_match('/status=(\S+)/', $out, $m) ? $m[1] : '?';
    $bytes   = preg_match('/bytes=(\d+)/', $out, $m) ? (int) $m[1] : -1;
    $outcome = preg_match('/outcome=(.+)$/m', $out, $m) ? trim($m[1]) : '?';

    echo "status: $status\n";
    echo "outcome: $outcome\n";
    echo "body arrived: ", $bytes > 0 ? 'yes' : "no ($bytes)", "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECTF--
%Astatus: 200
outcome: RESET(err=258)
body arrived: yes
%ADone
