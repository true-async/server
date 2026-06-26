--TEST--
HttpServer: HTTP/3 client rotates DCID to a server-issued CID — server must route it (RFC 9000 §5.1, #80)
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
/* CID rotation, NOT a NAT rebind. h3client issues request #1, then performs a
 * real client-initiated migration (H3CLIENT_ROTATE_DCID_AFTER=1): ngtcp2 picks
 * one of the server-issued NEW_CONNECTION_ID values as its new DCID and sends
 * request #2 addressed to it. Per RFC 9000 §5.1 a client may address the server
 * by ANY connection ID the server has issued. The server therefore MUST track
 * the CIDs it hands out in its lookup table, or the rotated DCID misses, the
 * packet is answered with a stateless reset, and the connection dies — request
 * #2 never completes.
 *
 * Before the conn_map fix this test fails (ok_responses=1, completed_both=0).
 * After it, both requests succeed on the one connection.
 *
 * Single-worker scope (default): CID steering across reactors is exercised
 * separately in 040; here we isolate the plain per-listener conn_map lookup. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-042';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 21420 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('rotate-ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(120000);
    $cmd = sprintf('H3CLIENT_REQUEST_COUNT=2 H3CLIENT_ROTATE_DCID_AFTER=1 '
        . 'H3CLIENT_DEADLINE_MS=6000 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    echo "rotated_marker=", (str_contains($out, 'ROTATED_DCID') ? 1 : 0), "\n";
    echo "ok_responses=",   substr_count($out, 'STATUS=200'), "\n";
    echo "completed_both=",  (str_contains($out, 'COMPLETED=2') ? 1 : 0), "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "conn_accepted=",   (int)($s['quic_conn_accepted']    ?? -1), "\n";
    echo "migrations_ge1=",  ((int)($s['quic_path_migrations'] ?? 0) >= 1 ? 1 : 0), "\n";
    echo "cid_issued_ge1=",  ((int)($s['quic_new_cid_issued']  ?? 0) >= 1 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
rotated_marker=1
ok_responses=2
completed_both=1
conn_accepted=1
migrations_ge1=1
cid_issued_ge1=1
done
