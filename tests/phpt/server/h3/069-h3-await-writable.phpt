--TEST--
HttpServer: HTTP/3 awaitWritable() waits for the stream window instead of refusing
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
/* awaitWritable() parks the handler until the queue drains, on HTTP/3 as on
 * HTTP/1, HTTP/2 and the pool worker. A transport that refuses at once answers
 * "still full" for a stream that has room a millisecond later, and a handler
 * looping on that answer spins its thread instead of yielding it.
 *
 * The probe caps its window, so the first chunk fills nghttp3 and the second
 * stays on the queue behind a blocked stream. The wait either brings the room
 * back or reports there is none, and only the first lets the second chunk
 * reach the client. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-069';
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
    ->setReadTimeout(15)->setWriteTimeout(15);

$server = new HttpServer($config);

$CHUNK = 256 * 1024;

$server->addHttpHandler(function ($req, $res) use ($CHUNK) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'application/octet-stream');
    $res->setNoCompression();

    $chunk = str_repeat('a', $CHUNK);

    /* Reaches nghttp3 in one read; the window lets only a fraction of it out. */
    $res->write($chunk);

    /* Queued behind it: nghttp3 still holds the first chunk, so the reader is
     * not called and the bytes stay pending. */
    $queued = $res->tryWrite($chunk);

    echo 'queued=', (int) $queued, "\n";
    echo 'room=',   (int) $res->awaitWritable(5000), "\n";

    $res->end();
});

spawn(function () use ($server, $port, $probe, $CHUNK) {
    usleep(300000);

    $cmd = sprintf('python3 %s 127.0.0.1 %d /stream 16384 2>/dev/null',
        escapeshellarg($probe), $port);
    $out = shell_exec($cmd) ?? '';

    $bytes = preg_match('/bytes=(\d+)/', $out, $m) ? (int) $m[1] : -1;

    echo 'body=', ($bytes === 2 * $CHUNK) ? 'full' : "short ($bytes)", "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
queued=1
room=1
body=full
Done
