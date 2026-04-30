--TEST--
HttpServer: HTTP/3 per-peer connection budget rejects flood from one IP (Step 6f)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* Step 6f — per-peer-IP concurrent connection budget.
 *
 * Server is launched with PHP_HTTP3_PEER_BUDGET=1 so a single source
 * IP may have at most one live H3 conn at a time. Handler delays
 * 600ms so the first client holds the slot. Second h3client (same
 * 127.0.0.1) sends its INITIAL while the slot is held: the listener
 * peer_inc returns false, accept refuses, quic_conn_per_peer_rejected
 * advances. Second client times out at its 800ms deadline and exits
 * non-zero — verifying via the server-side counter is the load-bearing
 * assertion (the h3client exit status is just signal, not contract). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-113';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

putenv('PHP_HTTP3_PEER_BUDGET=1');

$port = 20800 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    \Async\delay(600);
    $res->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$driver = spawn(function () use ($server, $port, $client_bin) {
    \Async\delay(80);

    /* Client A — slow handler will hold the single peer slot. */
    $a = spawn(function () use ($client_bin, $port) {
        $cmd = sprintf('%s 127.0.0.1 %d / GET 2>&1',
            escapeshellarg($client_bin), $port);
        return shell_exec($cmd) ?? '';
    });

    /* Brief delay so A's INITIAL has been accepted before B fires. */
    \Async\delay(150);

    $b = spawn(function () use ($client_bin, $port) {
        $cmd = sprintf('H3CLIENT_DEADLINE_MS=800 %s 127.0.0.1 %d / GET 2>&1',
            escapeshellarg($client_bin), $port);
        return shell_exec($cmd) ?? '';
    });

    $out_b = await($b);
    $out_a = await($a);

    $sa = preg_match('/^STATUS=(\d+)$/m', $out_a, $m) ? (int)$m[1] : -1;
    $sb = preg_match('/^STATUS=(\d+)$/m', $out_b, $m) ? (int)$m[1] : -1;
    echo "client_a_status=$sa\n";
    echo "client_b_status=$sb\n";          /* expected -1 (timeout) */

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "per_peer_rejected_ge1=",
        ((int)($s['quic_conn_per_peer_rejected'] ?? 0) >= 1 ? 1 : 0), "\n";
    echo "conn_accepted_ge1=",
        ((int)($s['quic_conn_accepted'] ?? 0) >= 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($driver);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
client_a_status=200
client_b_status=-1
per_peer_rejected_ge1=1
conn_accepted_ge1=1
done
