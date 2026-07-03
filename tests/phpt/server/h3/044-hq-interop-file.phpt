--TEST--
HttpServer: hq-interop (HTTP/0.9-over-QUIC) serves files byte-exact + multiplexes (#80)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--ENV--
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* hq-interop is the raw HTTP/0.9-over-QUIC shim the quic-interop-runner
 * negotiates for the whole transport matrix (no nghttp3). This pins the
 * application behaviour the runner relies on:
 *   - byte-exact file delivery across sizes incl. one (128 KiB) that
 *     exceeds the initial congestion window — proving the ACK-driven
 *     drain resume (acked_stream_data_offset_cb cwnd-wake),
 *   - several concurrent request streams on one connection (multiplexing).
 * The client is aioquic (the runner's own QUIC stack); h3client can't speak
 * hq because it is nghttp3-only. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-044';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$root = $tmp . '/root';
@mkdir($root, 0700, true);

require __DIR__ . '/_h3_skipif.inc';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

/* Position-varying content catches offset/truncation bugs a uniform fill
 * would hide. 0 is the empty-body FIN-only case; 131072 (128 KiB) exceeds the
 * initial window so it crosses several ACK-driven drain resumes. */
$sizes = [0, 1, 16384, 131072];
$sha = [];
foreach ($sizes as $n) {
    $buf = '';
    for ($i = 0; $i < $n; $i++) $buf .= chr(($i * 31 + 7) & 0xff);
    file_put_contents("$root/f$n.bin", $buf);
    $sha[$n] = sha1($buf);
}

register_shutdown_function(function () use ($tmp, $root, $sizes) {
    foreach ($sizes as $n) @unlink("$root/f$n.bin");
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem");
    @rmdir($root); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
/* hq serves these files straight off the transport reactor. */
$config->setHttp3HqDocroot($root);

$server = new HttpServer($config);
/* An h3 handler is still required by start(); hq never reaches it. */
$server->addHttpHandler(function ($req, $res) { $res->setBody('h3'); });

$py     = __DIR__ . '/_hq_client.py';
$python = 'python3';

$client = spawn(function () use ($server, $port, $py, $python, $sizes, $sha) {
    usleep(200000);

    foreach ($sizes as $n) {
        $cmd = sprintf('%s %s 127.0.0.1 %d /f%d.bin 2>/dev/null',
            escapeshellarg($python), escapeshellarg($py), $port, $n);
        $body = shell_exec($cmd) ?? '';
        $ok = (strlen($body) === $n) && (sha1($body) === $sha[$n]);
        printf("size=%d len=%d match=%d\n", $n, strlen($body), $ok ? 1 : 0);
    }

    /* Multiplexing: 3 concurrent streams on one connection. */
    $cmd = sprintf('%s %s 127.0.0.1 %d --mux /f1.bin /f16384.bin /f131072.bin 2>/dev/null',
        escapeshellarg($python), escapeshellarg($py), $port);
    $lines = array_values(array_filter(explode("\n", shell_exec($cmd) ?? '')));
    $want = ['/f1.bin' => [1, $sha[1]],
             '/f16384.bin' => [16384, $sha[16384]],
             '/f131072.bin' => [131072, $sha[131072]]];
    $hit = 0;
    foreach ($lines as $ln) {
        [$p, $len, $h] = explode(' ', $ln) + [null, null, null];
        if (isset($want[$p]) && (int)$len === $want[$p][0] && $h === $want[$p][1]) $hit++;
    }
    printf("mux match=%d/%d\n", $hit, count($want));

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
size=0 len=0 match=1
size=1 len=1 match=1
size=16384 len=16384 match=1
size=131072 len=131072 match=1
mux match=3/3
done
