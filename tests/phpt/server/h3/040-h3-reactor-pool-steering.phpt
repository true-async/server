--TEST--
HttpServer: HTTP/3 CID steering — migrated client served across the reactor split (#80 D6 / #72)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
$n = (int) @shell_exec('nproc 2>/dev/null');
if ($n < 2) die('skip CID steering needs >1 reactor (>=2 cores)');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* CID steering (#80 D6 / #72).
 *
 * With the reactor pool and setWorkers(2), each transport reactor binds its own
 * SO_REUSEPORT socket; the kernel hashes datagrams across them by 4-tuple. A
 * client that NAT-rebinds (new source port) is therefore rehashed onto a reactor
 * that does NOT own its QUIC connection — the case 032 documents as broken with
 * setWorkers>1. Steering encodes the owner reactor's id into every server CID; a
 * reactor receiving a stray short-header datagram decodes the owner from the DCID
 * and forwards it over the reactor mailbox to the owner, which feeds it to ngtcp2
 * and replies directly.
 *
 * The client rebinds twice on one connection. With two reactors a rebind lands on
 * the non-owner ~half the time, so steering is almost always exercised — and
 * WITHOUT it, the first cross-reactor rebind would strand the connection and drop
 * the later responses. Asserting all three responses land (with workers=2, one
 * connection, the migration counter advanced) is therefore a steering regression
 * gate: it fails the moment a rehashed datagram is not routed home. The encode/
 * decode addressing itself is proven deterministically in the HTTP3Steer unit
 * test; the steering counters echoed below make the forward observable.
 *
 * Multi-worker scope: the gated pool only engages at setWorkers>1, so there is
 * no clean in-process stop (issue #11) — SIGKILL after the client; %A swallows
 * the abrupt exit. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-040';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 21340 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('steer-ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin) {
    /* Reactors + workers need a moment to thread up and bind. */
    usleep(700000);

    /* One connection, three requests, rebinding before requests 2 and 3. */
    $cmd = sprintf(
        'H3CLIENT_REQUEST_COUNT=3 H3CLIENT_MIGRATE_AFTER=1 H3CLIENT_DEADLINE_MS=6000 '
        . '%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    echo "ok_responses=",  substr_count($out, 'STATUS=200'), "\n";
    echo "migrated=",       (substr_count($out, 'MIGRATED') >= 1 ? 1 : 0), "\n";

    /* Aggregate across the per-reactor listener entries. */
    $accepted = 0; $steered = 0; $migr = 0;
    foreach ($server->getHttp3Stats() as $st) {
        $accepted += (int)($st['quic_conn_accepted']  ?? 0);
        $steered  += (int)($st['quic_steered_out']     ?? 0)
                   + (int)($st['quic_steered_in']       ?? 0);
        $migr     += (int)($st['quic_path_migrations']  ?? 0);
    }
    echo "conn_accepted=",  $accepted, "\n";
    echo "migrations_ge1=", ($migr >= 1 ? 1 : 0), "\n";
    /* Observability only (kernel reuseport may keep both rebinds on the owner). */
    fwrite(STDERR, "steered_total=$steered\n");

    /* Issue #11: no clean cross-thread shutdown for the pool yet. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Aok_responses=3
migrated=1
conn_accepted=1
migrations_ge1=1
%A
