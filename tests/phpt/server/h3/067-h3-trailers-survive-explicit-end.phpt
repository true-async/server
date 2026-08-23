--TEST--
HTTP/3 — trailers reach the peer when the handler ends the stream itself
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
/* `end()` reaches h3_stream_mark_ended, which resumes the stream and drains, so
 * the data reader can hit EOF inside that call. The trailers were captured only
 * afterwards, in the dispose, and by then the fin had gone — the peer read a
 * clean end with no trailer section. A stream the dispose finishes kept its
 * trailers, which is why the shape below is the one nothing covered.
 *
 * Both orderings are checked: the trailer set before the write and after it. The
 * handler cannot be expected to know that the position matters, and it does not.
 *
 * The client is aioquic, which reads H3 trailers; the C h3client cannot. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = sys_get_temp_dir() . '/h3-trailers-end-' . getmypid();
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
    $res->setStatusCode(200)->setHeader('content-type', 'text/plain');

    if ($req->getUri() === '/late') {
        $res->write('chunk');
        $res->setTrailer('x-probe-trailer', 'kept');
        $res->end();
        return;
    }

    $res->setTrailer('x-probe-trailer', 'kept');
    $res->write('chunk');
    $res->end();
});

spawn(function () use ($port, $server) {
    delay(400);

    $py = __DIR__ . '/../grpc/_h3grpc_client.py';

    foreach (['/early', '/late'] as $path) {
        $out = (string) shell_exec(sprintf(
            "python3 %s 127.0.0.1 %d %s text/plain '' 2>/dev/null",
            escapeshellarg($py), $port, escapeshellarg($path)));

        $body = '';

        if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m)) {
            $body = hex2bin($m[1]);
        }

        printf("%s status200=%d body=%s trailer=%d\n", $path,
            (int)(strpos($out, 'HDR :status: 200') !== false),
            $body,
            (int)(strpos($out, 'HDR x-probe-trailer: kept') !== false));
    }

    delay(200);
    $server->stop();
});

$server->start();
?>
--EXPECT--
/early status200=1 body=chunk trailer=1
/late status200=1 body=chunk trailer=1
