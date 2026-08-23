--TEST--
HTTP/3 — a request object the handler kept survives listener teardown
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
/* An HTTP/3 stream lives in a slab slot, and the slot comes back only when the
 * PHP HttpRequest wrapper is collected — the handler is free to keep it, and
 * `http3_stream_release` says so. The listener freed the slab anyway, so the
 * slot was pushed onto a freelist inside memory that had already been efree'd:
 * a debug build aborts on the live-slot assertion at teardown, and a release
 * build writes into the freed listener.
 *
 * The kept request is read after stop() as well: it must still answer, because
 * the wrapper is what the slot's lifetime now hangs on. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = sys_get_temp_dir() . '/h3-kept-req-' . getmypid();
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";

if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($dir, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($dir);
});

$port = tas_free_port();
$kept = [];

$server = new HttpServer(
    (new HttpServerConfig())
        ->addHttp3Listener('127.0.0.1', $port)
        ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
        ->setReadTimeout(10)->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) use (&$kept) {
    $kept[] = $req;
    $res->setStatusCode(200)->setBody('ok');
});

spawn(function () use ($port, $server, &$kept) {
    delay(400);

    $probe = __DIR__ . '/../../../h3client/h3probe.py';
    echo trim((string) shell_exec(sprintf(
        'python3 %s 127.0.0.1 %d /kept 2>/dev/null',
        escapeshellarg($probe), $port))), "\n";

    delay(200);
    $server->stop();

    echo 'kept=', count($kept), ' uri=', $kept[0]->getUri(), "\n";
});

$server->start();
echo "Done\n";
?>
--EXPECT--
status=200 bytes=2 cl=2 outcome=CLEAN_END
kept=1 uri=/kept
Done
