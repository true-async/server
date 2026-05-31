--TEST--
HttpServer: HTTP/3 connection migration — client NAT-rebind keeps one connection (RFC 9000 §9, #59)
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
/* Connection migration / NAT rebinding. h3client issues request #1, then
 * rebinds its UDP socket to a new source port (H3CLIENT_MIGRATE_AFTER=1)
 * and issues request #2 from there — the same QUIC connection (same CIDs),
 * a new 4-tuple. The server must keep the one connection alive: feed the
 * datagram's real source into ngtcp2 so it validates the new path and
 * re-point conn->peer at it. Asserts both requests succeed, the server
 * counted exactly one accepted connection (not a second accept), and the
 * migration counter advanced.
 *
 * Single-worker scope (default): with setWorkers>1 the SO_REUSEPORT rehash
 * would route the rebound 4-tuple to a different worker that does not own the
 * connection — that needs CID-steering (eBPF), tracked separately. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-032';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 21100 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('migrate-ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(120000);
    $cmd = sprintf('H3CLIENT_REQUEST_COUNT=2 H3CLIENT_MIGRATE_AFTER=1 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    echo "ok_responses=",  substr_count($out, 'STATUS=200'), "\n";
    echo "migrated_marker=", (str_contains($out, 'MIGRATED') ? 1 : 0), "\n";
    echo "completed_both=",  (str_contains($out, 'COMPLETED=2') ? 1 : 0), "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "conn_accepted=",   (int)($s['quic_conn_accepted']   ?? -1), "\n";
    echo "migrations_ge1=",  ((int)($s['quic_path_migrations'] ?? 0) >= 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
ok_responses=2
migrated_marker=1
completed_both=1
conn_accepted=1
migrations_ge1=1
done
