--TEST--
HttpServer: HTTP/3 idle timeout reaps the silent connection (Step 6e)
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
/* Step 6e — RFC 9000 §10.1 idle timeout, server side.
 *
 * Server runs with PHP_HTTP3_IDLE_TIMEOUT_MS=500 so ngtcp2 stamps a
 * 500ms idle timer in transport_params. h3client completes the GET
 * but is launched with H3CLIENT_NO_CLOSE=1 so it exits silently,
 * leaving the server-side conn idle. The retransmission timer fires
 * past the 500ms mark, ngtcp2_conn_handle_expiry returns
 * NGTCP2_ERR_IDLE_CLOSE, and timer_fire_cb reaps the connection
 * without emitting CONNECTION_CLOSE (per spec). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-112';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

putenv('PHP_HTTP3_IDLE_TIMEOUT_MS=500');

$port = 20700 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('ok'); });

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    \Async\delay(80);
    $cmd = sprintf('H3CLIENT_NO_CLOSE=1 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';
    $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
    echo "status=$status\n";

    /* Wait past the 500ms idle window + a margin for the timer cb to
     * fire and run the reap path. */
    \Async\delay(900);

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "idle_closed=", (int)($s['quic_conn_idle_closed']        ?? -1), "\n";
    echo "reaped=",      (int)($s['quic_conn_reaped']             ?? -1), "\n";
    echo "close_sent=",  (int)($s['quic_connection_close_sent']    ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
idle_closed=1
reaped=1
close_sent=0
done
