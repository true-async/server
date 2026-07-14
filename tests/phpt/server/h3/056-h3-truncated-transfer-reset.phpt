--TEST--
HttpServer: HTTP/3 truncated transfer resets the stream, not a clean EOF (aioquic, #123)
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
/* The truncated-transfer bug (5af8f7f): a sendFile whose file is truncated
 * after the headers are on the wire used to end the stream cleanly — the peer
 * saw a 200 with a short body and NO error, i.e. silent data loss. The fix
 * RESETs the write side instead (H3_INTERNAL_ERROR = 258), the only way HTTP/3
 * can say "this transfer failed" once a status + Content-Length have gone out.
 *
 * Our in-tree h3client cannot tell a clean end from a RESET_STREAM, so this is
 * proved with aioquic (tests/h3client/h3probe.py) — a separate QUIC stack.
 *
 * Determinism: the probe caps its flow-control window (65536), so the server
 * cannot race the whole 2 MiB body out; it paces against the window. The probe
 * writes its running byte count to a progress file, and the test truncates the
 * file to 4 KiB the instant it has seen a chunk arrive — so the server is
 * provably mid-body (well under 2 MiB) when the file shrinks under it, and its
 * next read hits EOF before body_length → RESET. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-056';
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";
$file = "$dir/big.bin";
$prog = "$dir/progress";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$SIZE = 2 * 1024 * 1024;
file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', $SIZE / 16));

register_shutdown_function(function () use ($dir, $cert, $key, $file, $prog) {
    @unlink($cert); @unlink($key); @unlink($file); @unlink($prog); @rmdir($dir);
});

$probe = __DIR__ . '/../../../h3client/h3probe.py';
$port  = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($file) { $res->sendFile($file); });

spawn(function () use ($server, $port, $probe, $file, $prog, $SIZE) {
    usleep(300000);
    @unlink($prog);

    $cmd = sprintf('python3 %s 127.0.0.1 %d /big 65536 %s 2>/dev/null',
        escapeshellarg($probe), $port, escapeshellarg($prog));
    $pipes = [];
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);

    /* Truncate once the transfer is demonstrably underway (>= 256 KiB in, still
     * far short of 2 MiB), then let the probe run to its verdict. */
    $truncated = false;
    for ($i = 0; $i < 600; $i++) {
        if (!$truncated && (int) @file_get_contents($prog) >= 262144) {
            $fp = fopen($file, 'r+');
            ftruncate($fp, 4096);
            fclose($fp);
            $truncated = true;
        }
        if (!proc_get_status($p)['running']) break;
        usleep(10000);
    }

    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]); fclose($pipes[2]);
    proc_close($p);

    $status  = preg_match('/status=(\S+)/', $out, $m) ? $m[1] : '?';
    $bytes   = preg_match('/bytes=(\d+)/', $out, $m) ? (int) $m[1] : -1;
    $outcome = preg_match('/outcome=(.+)$/m', $out, $m) ? trim($m[1]) : '?';

    echo "truncated: ", $truncated ? 'yes' : 'NO', "\n";
    echo "status: $status\n";
    echo "outcome: $outcome\n";
    echo "partial body: ", ($bytes > 0 && $bytes < $SIZE) ? 'yes' : "no ($bytes)", "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
truncated: yes
status: 200
outcome: RESET(err=258)
partial body: yes
Done
