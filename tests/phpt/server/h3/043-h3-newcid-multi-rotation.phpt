--TEST--
HttpServer: HTTP/3 client rotates DCID several times in sequence — every issued CID routes (RFC 9000 §5.1, #80)
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
/* Like 042 but exercises MULTIPLE sequential DCID rotations on one connection
 * (REQUEST_COUNT=4, ROTATE_DCID_AFTER=1 → 3 rotations). Each rotation makes
 * ngtcp2 adopt another server-issued CID, so the per-connection issued-CID
 * record carries several live entries (each registered in conn_map). Asserts
 * all four requests succeed and the issued-CID counter advanced past the
 * родовые keys. (The RETIRE path — remove_connection_id → quic_cid_retired —
 * is not asserted here: ngtcp2's client retires old CIDs on its own schedule,
 * not deterministically within the window; that path's safety is covered under
 * ASan.) Single-worker: isolates conn_map from reactor steering. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-043';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 21460 + getmypid() % 40;

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
    $cmd = sprintf('H3CLIENT_REQUEST_COUNT=4 H3CLIENT_ROTATE_DCID_AFTER=1 '
        . 'H3CLIENT_DEADLINE_MS=8000 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    echo "rotations=",     substr_count($out, 'ROTATED_DCID'), "\n";
    echo "ok_responses=",  substr_count($out, 'STATUS=200'), "\n";
    echo "completed_all=", (str_contains($out, 'COMPLETED=4') ? 1 : 0), "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "conn_accepted=",   (int)($s['quic_conn_accepted']   ?? -1), "\n";
    echo "cid_issued_ge3=",  ((int)($s['quic_new_cid_issued'] ?? 0) >= 3 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
rotations=3
ok_responses=4
completed_all=1
conn_accepted=1
cid_issued_ge3=1
done
