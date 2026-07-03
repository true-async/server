--TEST--
HttpServerConfig: setTlsBufferBytes — record rounding, clamps, locked guard, functional (#29)
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
/* The CT-out TLS BIO ring is tunable via setTlsBufferBytes (issue #29).
 * The value is rounded UP to whole TLS records (~17 KiB = 16 KiB payload +
 * AEAD/header overhead) so the ring always holds whole records, floored at
 * one record, capped at 16, with 0 resetting to the 64 KiB default.
 * getTlsBufferBytes() returns the effective (rounded) size. */

require_once __DIR__ . '/_tls_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

/* === Part A: record rounding + default === */
$c = new HttpServerConfig();
echo "default=" . $c->getTlsBufferBytes() . "\n";          /* 65536 */
foreach ([16384, 17408, 17409, 33000, 65536, 270000, 278528] as $set) {
    $c->setTlsBufferBytes($set);
    echo "set $set -> " . $c->getTlsBufferBytes() . "\n";
}
$c->setTlsBufferBytes(0);
echo "set 0 -> " . $c->getTlsBufferBytes() . "\n";          /* 65536 (default) */

/* === Part B: clamps reject out-of-range === */
$reject = function (callable $fn, string $label) {
    try { $fn(); echo "$label ACCEPTED\n"; }
    catch (Throwable $e) { echo "$label rejected\n"; }
};
$reject(fn() => $c->setTlsBufferBytes(-1),     "neg");
$reject(fn() => $c->setTlsBufferBytes(278529), "over-max");

/* === Part C: locked after the config is handed to a server === */
$cert = sys_get_temp_dir() . '/tls015-' . getmypid() . '.crt';
$key  = sys_get_temp_dir() . '/tls015-' . getmypid() . '.key';
if (!tls_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($cert, $key) { @unlink($cert); @unlink($key); });

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$size = 200000;                                            /* > one record → exercises park path */
$body = str_repeat("\0", $size);
for ($i = 0; $i < $size; $i++) { $body[$i] = chr(($i * 31 + 7) & 0xff); }
$want_sha = sha1($body);

$cfg = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setTlsBufferBytes(17408)                             /* one record — smallest ring */
    ->setReadTimeout(10)->setWriteTimeout(10);
$server = new HttpServer($cfg);
echo "effective_ring=" . $cfg->getTlsBufferBytes() . "\n"; /* 17408 */
$reject(fn() => $cfg->setTlsBufferBytes(65536), "locked");

$server->addHttpHandler(function ($req, $res) use ($body) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream')->setBody($body);
});

/* === Part D: functional — a multi-record body serves correctly over a
 * one-record ring (the #29 park path, driven by the setter). === */
$client = spawn(function () use ($port, $size, $want_sha) {
    usleep(120000);
    $out = sys_get_temp_dir() . '/tls015_body.bin';
    @unlink($out);
    $cmd = sprintf('curl -sk --http2 -m 8 -o %s -w "%%{http_code}" https://127.0.0.1:%d/ 2>&1',
        escapeshellarg($out), $port);
    $code = shell_exec($cmd);
    $ok = is_file($out) && filesize($out) === $size && hash_file('sha1', $out) === $want_sha;
    @unlink($out);
    return [$code, $ok];
});
spawn(function () use ($server) { usleep(7000000); if ($server->isRunning()) $server->stop(); });

$server->start();
[$code, $ok] = await($client);
if ($server->isRunning()) $server->stop();

echo "serve: code=$code integrity=" . ($ok ? "ok" : "bad") . "\n";
echo "done\n";
?>
--EXPECT--
default=65536
set 16384 -> 17408
set 17408 -> 17408
set 17409 -> 34816
set 33000 -> 34816
set 65536 -> 69632
set 270000 -> 278528
set 278528 -> 278528
set 0 -> 65536
neg rejected
over-max rejected
effective_ring=17408
locked rejected
serve: code=200 integrity=ok
done
