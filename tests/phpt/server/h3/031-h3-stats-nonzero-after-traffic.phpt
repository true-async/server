--TEST--
HttpServer: HTTP/3 success-path stats counters advance non-zero after a GET (#59)
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
/* Several success-path counters are only ever asserted == 0 (008), so a
 * regression that stopped incrementing them would pass the suite. This
 * drives one full GET round-trip and asserts the handshake / nghttp3-init
 * / egress counters are actually non-zero: a live "the wiring works" gate,
 * not just a schema probe. (quic_timer_fired is deliberately omitted — it
 * is not deterministically fired inside the request window.) */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-031';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 20560 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('h3-stats');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(100000);
    $out = shell_exec(sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port)) ?? '';
    $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
    echo "status=", $status, "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "datagrams_received_ge1=",   ((int)($s['datagrams_received']      ?? 0) >= 1 ? 1 : 0), "\n";
    echo "handshake_completed_ge1=",  ((int)($s['quic_handshake_completed'] ?? 0) >= 1 ? 1 : 0), "\n";
    echo "h3_init_ok_ge1=",           ((int)($s['h3_init_ok']               ?? 0) >= 1 ? 1 : 0), "\n";
    echo "packets_sent_ge1=",         ((int)($s['quic_packets_sent']        ?? 0) >= 1 ? 1 : 0), "\n";
    echo "bytes_sent_gt0=",           ((int)($s['quic_bytes_sent']          ?? 0) >  0 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
status=200
datagrams_received_ge1=1
handshake_completed_ge1=1
h3_init_ok_ge1=1
packets_sent_ge1=1
bytes_sent_gt0=1
done
