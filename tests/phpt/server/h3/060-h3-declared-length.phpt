--TEST--
HttpServer: a declared Content-Length reaches the HTTP/3 peer, and a short body resets the stream
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--FILE--
<?php
/* The HTTP/3 half of the same contract 054 pins for HTTP/2: a length the
 * handler declared is carried in the field section, and the server holds the
 * body to it. Read with aioquic rather than the in-tree h3client for the
 * reason 058 gives — h3client shares ngtcp2 and nghttp3 with the server, and a
 * shared misreading would hide the defect from both.
 *
 * The undeclared route is the control: a stream framed by its own end still
 * carries no length, so what the declared route proves is the declaration and
 * not some blanket change to the field section. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-060';
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
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    switch ($req->getUri()) {
        case '/declared':
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setHeader('Content-Length', '9');
            $res->write('alpha');
            $res->end('beta');
            return;

        case '/short':
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setHeader('Content-Length', '100');
            $res->write('alpha');
            $res->end();
            return;
    }

    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('alpha');
    $res->end('beta');
});

spawn(function () use ($server, $port, $probe) {
    usleep(300000);

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

    [$status, $bytes, $cl, $outcome] = $run('/short');
    echo "short: status=$status cl=$cl bytes=$bytes outcome=$outcome\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
declared: status=200 cl=9 bytes=9 outcome=CLEAN_END
undeclared: status=200 cl=- bytes=9 outcome=CLEAN_END
short: status=200 cl=100 bytes=5 outcome=RESET(err=258)
Done
