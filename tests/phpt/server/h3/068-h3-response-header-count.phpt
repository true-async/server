--TEST--
HTTP/3 — a response with 300 headers keeps all of them and its Content-Length
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
/* The direct path stopped emitting header fields at 256 and told nobody: the
 * flatten loop left through `goto headers_done` and the response went out
 * truncated. Whatever the hash order had put past the cap was gone, and
 * `content-length` with it, so the body arrived in full with no framing — the
 * server dropping the number it had computed one line earlier.
 *
 * The reactor pool and HTTP/2 never had the cap, so the same handler answered
 * three different ways depending on which path served it. The assertion is that
 * it no longer does. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = sys_get_temp_dir() . '/h3-header-count-' . getmypid();
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";

if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($dir, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($dir);
});

$port = tas_free_port();
$pad  = 300;
$size = 512;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addHttp3Listener('127.0.0.1', $port)
        ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
        ->setReadTimeout(10)->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use ($pad, $size) {
    $res->setStatusCode(200)->setHeader('content-type', 'text/plain');

    for ($i = 0; $i < $pad; $i++) {
        $res->setHeader(sprintf('x-pad-%03d', $i), 'v');
    }

    $res->setBody(str_repeat('b', $size));
});

spawn(function () use ($port, $server, $pad, $size) {
    delay(400);

    $py  = __DIR__ . '/../grpc/_h3grpc_client.py';
    $out = (string) shell_exec(sprintf(
        "python3 %s 127.0.0.1 %d /pad text/plain '' 2>/dev/null",
        escapeshellarg($py), $port));

    $body = '';

    if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m)) {
        $body = hex2bin($m[1]);
    }

    printf("arrived=%d of %d\n", preg_match_all('/^HDR x-pad-\d{3}: v$/m', $out), $pad);
    printf("content_length=%d body_len=%d\n",
        (int)(strpos($out, 'HDR content-length: ' . $size) !== false), strlen($body));

    delay(200);
    $server->stop();
});

$server->start();
?>
--EXPECT--
arrived=300 of 300
content_length=1 body_len=512
