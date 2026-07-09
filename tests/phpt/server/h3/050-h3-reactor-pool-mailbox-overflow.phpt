--TEST--
HttpServer: reverse-mailbox overflow defers wires + slot release, body stays byte-exact (#106 follow-up)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-050';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$chunks = 150;
$expected = '';
for ($i = 1; $i <= $chunks; $i++) {
    $expected .= "chunk{$i};";
}

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2)
    /* Floor the reverse mailbox at its minimum (64). A 150-chunk reply cannot
     * fit, so most STREAM_CHUNKs and the final stream-slot release take the
     * worker's defer/retry FIFO instead of a direct post — exercising
     * pending_post_defer / pending_post_flush and the consumed-release-through-
     * FIFO path. A byte-exact, in-order body proves the FIFO holds ordering
     * under overflow and no slot leaks at teardown. */
    ->setReactorMailboxCapacity(64);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($chunks) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain; charset=utf-8');

    for ($i = 1; $i <= $chunks; $i++) {
        $res->send("chunk{$i};");
    }

    $res->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $expected) {
    usleep(600000);

    $cmd = sprintf('H3CLIENT_DEADLINE_MS=6000 %s 127.0.0.1 %d /stream GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out);

    echo "status=", $status ?? -1, "\n";
    echo "integrity=", (trim($body) === $expected ? "ok" : "MISMATCH"), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
%Astatus=200
integrity=ok
%A
