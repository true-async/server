--TEST--
StaticHandler: HTTP/2 over TLS — multi-record body integrity (issue #23 regression)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
$probe = shell_exec('curl --http2 -V 2>&1');
if (strpos($probe ?? '', 'HTTP2') === false && strpos($probe ?? '', 'http2') === false) {
    echo 'skip curl without HTTP/2 support';
    exit;
}
?>
--FILE--
<?php
require_once __DIR__ . '/../tls/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-013';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) { echo "cert failed\n"; exit(1); }

/* 64 KiB body — straddles the slurp threshold (<=64 KiB → inline-body
 * buffered path) AND forces SSL_write to span > 1 TLS record (>16 KiB
 * plaintext per record per RFC 8446). Multi-record is the trigger:
 * the pre-fix bug shipped one good record then corrupted bytes from
 * the cipher BIO ring on the second submit, manifesting as "bad
 * record mac" client-side. Single-record bodies (< 16 KiB) wouldn't
 * exercise it. */
$root = "$tmp/docroot";
@mkdir($root, 0700, true);
$body_size = 64 * 1024;
$blob = str_repeat("ABCDEFGH", $body_size / 8);
file_put_contents("$root/big.bin", $blob);

register_shutdown_function(function() use ($root, $tmp) {
    @unlink("$root/big.bin");
    @rmdir($root);
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem");
    foreach (glob("$tmp/body*.out") as $f) @unlink($f);
    @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(8)
    ->setWriteTimeout(8);

$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/s/', $root)));

$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->setStatusCode(204)->end();
    $server->stop();
});

$client = spawn(function () use ($port, $tmp, $body_size, $blob) {
    usleep(80000);
    /* Three sequential GETs over h2+TLS on fresh connections. Cipher
     * BIO ring (17 KiB) holds one ~16400-byte record at a time; the
     * second+ records have to come out cleanly after consume. Running
     * three iterations means at minimum 6 cipher-records get peeked,
     * shipped, consumed, repeated — pre-fix this dies on record #2. */
    $ok = 0;
    for ($i = 1; $i <= 3; $i++) {
        $out = "$tmp/body$i.out";
        $cmd = sprintf(
            'curl -sk --http2 -m 6 -o %s -w "HTTP=%%{http_code} BYTES=%%{size_download}" ' .
            'https://127.0.0.1:%d/s/big.bin 2>&1',
            escapeshellarg($out),
            $port
        );
        $meta = shell_exec($cmd);
        $got  = @file_get_contents($out);
        if (strpos($meta ?? '', 'HTTP=200') !== false
            && strpos($meta ?? '', "BYTES=$body_size") !== false
            && $got === $blob) {
            $ok++;
        }
    }
    @file_get_contents("https://127.0.0.1:$port/__stop", false, stream_context_create([
        'ssl' => ['verify_peer' => false, 'verify_peer_name' => false],
        'http' => ['timeout' => 2],
    ]));
    return $ok;
});

spawn(function () use ($server) {
    usleep(8000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$ok = await($client);

echo "all-ok: " . ($ok === 3 ? 'ok' : "bad($ok/3)") . "\n";
echo "Done\n";
?>
--EXPECT--
all-ok: ok
Done
