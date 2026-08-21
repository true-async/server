--TEST--
HttpServer: a declared Content-Length crosses the pool wire and frames the body (gated pool)
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
/* Under the pool the response is flattened on a worker thread and the reactor
 * submits what the wire carried, so a header the worker drops is gone before
 * the reactor could put it back. The worker strips content-length from every
 * head it copies — the length is implicit in DATA frames — and the streaming
 * wire is the one place that has to stop doing so.
 *
 * The undeclared route is the control: it crosses the same wire and still
 * arrives without a length. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-061';
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
    $res->setStatusCode(200)->setHeader('content-type', 'text/plain');

    if ($req->getUri() === '/declared') {
        $res->setHeader('Content-Length', '9');
    }

    $res->write('alpha');
    $res->end('beta');
});

spawn(function () use ($server, $port, $probe) {
    usleep(600000);

    $run = static function (string $path) use ($probe, $port) {
        $cmd = sprintf('python3 %s 127.0.0.1 %d %s 2>/dev/null',
            escapeshellarg($probe), $port, escapeshellarg($path));
        $out = (string) shell_exec($cmd);

        return [
            preg_match('/status=(\S+)/', $out, $m) ? $m[1] : '?',
            preg_match('/bytes=(\d+)/', $out, $m) ? (int) $m[1] : -1,
            preg_match('/cl=(\S+)/', $out, $m) ? $m[1] : '?',
            preg_match('/outcome=(.+)$/m', $out, $m) ? trim($m[1]) : '?',
        ];
    };

    [$status, $bytes, $cl, $outcome] = $run('/declared');
    echo "declared: status=$status cl=$cl bytes=$bytes outcome=$outcome\n";

    [$status, $bytes, $cl, $outcome] = $run('/undeclared');
    echo "undeclared: status=$status cl=$cl bytes=$bytes outcome=$outcome\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECTF--
%Adeclared: status=200 cl=9 bytes=9 outcome=CLEAN_END
undeclared: status=200 cl=- bytes=9 outcome=CLEAN_END
%ADone
