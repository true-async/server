--TEST--
HttpServer: HTTP/2 over TLS — large body survives a CT-out BIO ring smaller than the gather (issue #29)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'curl_h2' => true]);
?>
--FILE--
<?php
/* Regression for the TLS write deadlock (issue #29). The h2 GATHER emit
 * path SSL_writes up to H2_EMIT_TLS_BYTE_CAP (32 KiB) of plaintext per
 * pass straight into the CT-out BIO ring. When that ring is smaller than
 * the gather, SSL_write returns WANT_WRITE after a partial encrypt; the
 * old code treated that as failure and discarded the undelivered tail,
 * so the client got 200 + a truncated/zero body and hung until timeout.
 * The fix parks the tail in the PT queue and lets the existing drain +
 * cipher-completion chain finish encrypting it.
 *
 * At the default 64 KiB CT-out ring the gather (32 KiB) fits in one
 * SSL_write, so this test verifies large-body byte integrity over several
 * gather passes. To actually exercise the park/resume path, rebuild the
 * extension with the ring shrunk below the gather:
 *
 *     make EXTRA_CFLAGS=-DTLS_BIO_RING_SIZE=17408
 *
 * Before the fix that build hangs here on every body > one TLS record;
 * after the fix it passes. Content is position-varying so a reorder bug
 * in the park path (encrypting a later slice ahead of the parked tail)
 * shows up as a sha1 mismatch, not just a length change. */

require_once __DIR__ . '/_tls_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-014';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 19930 + getmypid() % 20;
$size = 256 * 1024;                       /* 8x the 32 KiB gather cap */
$body = str_repeat("\0", $size);
for ($i = 0; $i < $size; $i++) { $body[$i] = chr(($i * 31 + 7) & 0xff); }
$want_sha = sha1($body);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($body) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'application/octet-stream')
        ->setBody($body);
});

$client = spawn(function () use ($port, $size, $tmp) {
    usleep(120000);
    $out = $tmp . '/body.bin';
    @unlink($out);
    $cmd = sprintf(
        'curl -sk --http2 -m 8 -o %s '
        . '-w "HTTP=%%{http_code} V=%%{http_version} SIZE=%%{size_download}" '
        . 'https://127.0.0.1:%d/ 2>&1',
        escapeshellarg($out), $port
    );
    $meta = shell_exec($cmd);
    return [$meta, $out];
});

spawn(function () use ($server) {
    usleep(7000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
[$meta, $out_path] = await($client);
if ($server->isRunning()) { $server->stop(); }

echo "status: "  . (strpos($meta, 'HTTP=200')       !== false ? 'ok' : "bad ($meta)") . "\n";
echo "version: " . (strpos($meta, 'V=2')            !== false ? 'ok' : 'bad') . "\n";
echo "size: "    . (strpos($meta, 'SIZE=' . $size)  !== false ? 'ok' : 'bad') . "\n";
echo "content: " . (is_file($out_path) && filesize($out_path) === $size
                      && hash_file('sha1', $out_path) === $want_sha
                    ? 'ok' : 'bad') . "\n";

@unlink($out_path);
echo "done\n";
?>
--EXPECT--
status: ok
version: ok
size: ok
content: ok
done
