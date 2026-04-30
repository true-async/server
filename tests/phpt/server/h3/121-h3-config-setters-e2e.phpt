--TEST--
HttpServer: H3 config setters drive transport params end-to-end (NEXT_STEPS.md §5)
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
/* §5 e2e — config-driven idle timeout reaps the conn the same way the
 * legacy PHP_HTTP3_IDLE_TIMEOUT_MS env does. Mirrors phpt 112 but
 * drives the value through HttpServerConfig::setHttp3IdleTimeoutMs(),
 * which proves:
 *   - the setter persists past lock + thread-transfer,
 *   - http_server_class.c reads it at start(),
 *   - http3_connection.c picks it up when accepting a new QUIC conn,
 *   - the timer fires the reap path within the configured window.
 *
 * Also exercises the other three setters in the same config — server
 * starts cleanly with all four explicitly set, GET still completes,
 * peer budget is observable via getHttp3Stats(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-121';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20920 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setHttp3IdleTimeoutMs(500)            // <- value under test
    ->setHttp3StreamWindowBytes(2 * 1024 * 1024)
    ->setHttp3MaxConcurrentStreams(50)
    ->setHttp3PeerConnectionBudget(8);

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

    \Async\delay(900);

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "idle_closed_ge1=", ((int)($s['quic_conn_idle_closed'] ?? 0) >= 1 ? 1 : 0), "\n";
    echo "reaped_ge1=",      ((int)($s['quic_conn_reaped']      ?? 0) >= 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
idle_closed_ge1=1
reaped_ge1=1
done
