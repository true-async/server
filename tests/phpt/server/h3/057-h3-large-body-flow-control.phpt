--TEST--
HttpServer: HTTP/3 large body delivers in full under a small client window (aioquic, #123)
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
/* Flow-control on a large download, verified with an independent client. The
 * probe caps its window at 65536, so a 2 MiB body only completes if the server
 * refills the stream window ~32 times as the peer drains it — the path where
 * h3client's own credit bugs have masked server ones before (302b149). aioquic
 * counts every DATA byte and reports a clean stream end only on a real FIN, so
 * a short body or a stall would show as fewer bytes or a TIMEOUT, not a pass. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-057';
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";
$file = "$dir/big.bin";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$SIZE = 2 * 1024 * 1024;
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', $SIZE / 16));

register_shutdown_function(function () use ($dir, $cert, $key, $file) {
    @unlink($cert); @unlink($key); @unlink($file); @rmdir($dir);
});

$probe = __DIR__ . '/../../../h3client/h3probe.py';
$port  = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(15)->setWriteTimeout(15);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($file) { $res->sendFile($file); });

spawn(function () use ($server, $port, $probe, $SIZE) {
    usleep(300000);

    $cmd = sprintf('python3 %s 127.0.0.1 %d /big 65536 2>/dev/null',
        escapeshellarg($probe), $port);
    $out = shell_exec($cmd) ?? '';

    $status  = preg_match('/status=(\S+)/', $out, $m) ? $m[1] : '?';
    $bytes   = preg_match('/bytes=(\d+)/', $out, $m) ? (int) $m[1] : -1;
    $outcome = preg_match('/outcome=(.+)$/m', $out, $m) ? trim($m[1]) : '?';

    echo "status: $status\n";
    echo "outcome: $outcome\n";
    echo "full body: ", ($bytes === $SIZE) ? 'yes' : "no ($bytes)", "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
status: 200
outcome: CLEAN_END
full body: yes
Done
