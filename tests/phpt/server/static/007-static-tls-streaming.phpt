--TEST--
StaticHandler: 1 MiB file served over TLS via chunked SSL_write FSM (issue #13 §5a)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../tls/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/../tls/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-007';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) { echo "cert failed\n"; exit(1); }

/* 1 MiB synthetic body — non-zero pattern catches single-chunk-only
 * regressions where the loop terminates early. */
$root = "$tmp/docroot";
@mkdir($root, 0700, true);
$body_size = 1 * 1024 * 1024;
$blob = str_repeat("ABCDEFGH", $body_size / 8);   /* exactly $body_size bytes */
file_put_contents("$root/big.bin", $blob);

register_shutdown_function(function() use ($root, $tmp) {
    @unlink("$root/big.bin");
    @rmdir($root);
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem"); @unlink("$tmp/body.out");
    @rmdir($tmp);
});

$port = 19960 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(8)
    ->setWriteTimeout(8);

$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/s/', $root)));

/* Stop after one request. addHttpHandler is the only way to register
 * a stop hook that fires after the static response is on the wire,
 * but addHttpHandler+addStaticHandler can co-exist — static wins for
 * matching prefixes, the PHP handler runs only on /__stop. */
$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->setStatusCode(204)->end();
    $server->stop();
});

$out = "$tmp/body.out";

$client = spawn(function () use ($port, $out) {
    usleep(80000);
    /* -k skips self-signed verify; --http1.1 pins protocol;
     * -o writes body; -w yields the metadata line. */
    $cmd = sprintf(
        'curl -sk --http1.1 -m 6 -o %s -w "HTTP=%%{http_code} BYTES=%%{size_download} CT=%%{content_type}" ' .
        'https://127.0.0.1:%d/s/big.bin 2>&1',
        escapeshellarg($out),
        $port
    );
    $meta = shell_exec($cmd);
    /* Trigger stop now that the body is on disk. */
    @file_get_contents("https://127.0.0.1:$port/__stop", false, stream_context_create([
        'ssl' => ['verify_peer' => false, 'verify_peer_name' => false],
        'http' => ['timeout' => 2],
    ]));
    return $meta;
});

/* Hard safety net. */
spawn(function () use ($server) {
    usleep(4500000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$meta = await($client);

$got = @file_get_contents($out);
echo "status: "   . (strpos($meta ?? '', 'HTTP=200') !== false ? 'ok' : 'bad') . "\n";
echo "bytes: "    . (strpos($meta ?? '', "BYTES=$body_size") !== false ? 'ok' : 'bad') . "\n";
echo "ctype: "    . (stripos($meta ?? '', 'application/octet-stream') !== false ? 'ok' : 'bad') . "\n";
echo "byte-exact: " . ($got === $blob ? 'ok' : 'bad(' . strlen((string)$got) . '/' . $body_size . ')') . "\n";

/* The hard-zero counter must have bumped — TLS path now goes through
 * ss_kick_off too, so the counter is the regression guard for "did
 * the new chunked-encrypt FSM actually run". */
$tel = $server->getTelemetry();
echo "zerocoro: " . (isset($tel['static_zero_coroutine_total']) && $tel['static_zero_coroutine_total'] >= 1 ? 'ok' : 'bad') . "\n";

echo "Done\n";
--EXPECT--
status: ok
bytes: ok
ctype: ok
byte-exact: ok
zerocoro: ok
Done
