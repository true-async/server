--TEST--
HttpServer: 1 MiB POST round-trip over TLS — byte-exact
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-063';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert    = $tmp_dir . '/cert.pem';
$key     = $tmp_dir . '/key.pem';
$payload = $tmp_dir . '/payload.bin';
$echoed  = $tmp_dir . '/echoed.bin';
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

// PLAN_TLS step 5 calls for a 1 MiB byte-exact round trip.
$size = 1024 * 1024;
$buf = '';
$state = 'abcdefghij0123456789';
while (strlen($buf) < $size) {
    $buf  .= sha1($state, true);  // 20 bytes appended
    $state = sha1($state, true);  // chain on the 20-byte state, not the growing buf
}
$buf = substr($buf, 0, $size);
file_put_contents($payload, $buf);
$expected_sha1 = sha1($buf);

$port = 19920 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(8)
    ->setWriteTimeout(8);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    // Dispatch fires at on_headers_complete (see http_parser.c), so for
    // large bodies we must explicitly await the message-complete event.
    $req->awaitBody();
    $body = $req->getBody() ?? '';
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'application/octet-stream')
        ->setBody($body)
        ->end();
    // Do NOT stop() here: on Windows/IOCP, uv_stop() called from inside
    // a callback sets a "stop next iteration" flag.  A 1 MiB response
    // needs several IOCP completion rounds to flush; stopping too early
    // stalls those writes and curl never receives the full body.
    // Instead, stop() after shell_exec confirms curl finished (below).
});

$client = spawn(function () use ($port, $payload, $echoed, $server) {
    usleep(80000);
    $cmd = sprintf(
        'curl -sk --http1.1 -m 10 ' .
        '-H "Content-Type: application/octet-stream" ' .
        '--data-binary @%s -o %s -w "HTTP=%%{http_code}" ' .
        'https://127.0.0.1:%d/echo 2>&1',
        escapeshellarg($payload),
        escapeshellarg($echoed),
        $port
    );
    $out = shell_exec($cmd);
    $server->stop();
    return $out;
});

// Safety net.
spawn(function () use ($server) {
    usleep(7000000);  // 7 s
    if ($server->isRunning()) $server->stop();
});

$server->start();
$meta = await($client);

echo "status: " . (strpos($meta, 'HTTP=200') !== false ? 'ok' : 'bad') . "\n";
$got = @file_get_contents($echoed);
echo "size: " . ($got !== false && strlen($got) === $size ? 'ok' : 'bad (' . ($got === false ? 'no file' : strlen($got)) . ')') . "\n";
echo "sha1: " . ($got !== false && sha1($got) === $expected_sha1 ? 'ok' : 'bad') . "\n";

@unlink($cert); @unlink($key); @unlink($payload); @unlink($echoed);
@rmdir($tmp_dir);

echo "Done\n";
--EXPECT--
status: ok
size: ok
sha1: ok
Done
