--TEST--
HttpServer: a streaming HTTP/3 handler that throws resets the stream (#171)
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
/* Same failure as the truncated sendFile in 056, reached from the other side:
 * there the file ran out under a declared Content-Length, here the handler
 * throws after committing a chunked stream. Either way the status is already
 * on the wire and the body is short, and HTTP/3's only way to say so is to
 * reset the stream.
 *
 * Proved with aioquic rather than the in-tree h3client for the reason 056
 * gives: h3client shares ngtcp2 and nghttp3 with the server, so a shared
 * misreading would hide the bug from both. No pacing trick is needed — the
 * handler throws on its own, without waiting for the peer. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-058';
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
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write(str_repeat('a', 4096));

    throw new \RuntimeException('handler failed mid-body');
});

spawn(function () use ($server, $port, $probe) {
    usleep(300000);

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
--EXPECT--
status: 200
outcome: RESET(err=258)
body arrived: yes
Done
