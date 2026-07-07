--TEST--
HttpServer: HTTP/3 migration-storm guard — a rebind flood is shed, not hung (#80 D6)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
$n = (int) @shell_exec('nproc 2>/dev/null');
if ($n < 2) die('skip migration-storm guard exercised under the reactor pool (>=2 cores)');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Migration-storm guard (#80 D6).
 *
 * A client that NAT-rebinds faster than its path validates (RFC 9000 §9.3 lets
 * the server decline migration) wedges ngtcp2 path validation: the response
 * chases the just-abandoned path while the live path only gets PTO probes, and
 * the connection hangs to idle-timeout instead of progressing. The guard counts
 * migrations in a sliding window and, past the cap, sheds the connection with a
 * graceful CONNECTION_CLOSE to the live (migrated) address.
 *
 * The client here rebinds before EVERY request of a 16-request run (15 back-to-
 * back sub-millisecond migrations) — the pathological storm. We assert:
 *   - the connection served at least one request before the guard fired
 *     (the guard does not break normal early traffic);
 *   - quic_migration_storm_shed advanced (the guard actually shed it);
 *   - the client returned promptly (no multi-second hang) — the regression that
 *     would return the moment the shed path stops working.
 *
 * Legitimate migration (1-2 rebinds) stays well under the cap and is covered by
 * 032 / 040, which still pass unchanged.
 *
 * Multi-worker scope: the gated pool only engages at setWorkers>1, so there is
 * no clean in-process stop (issue #11) — SIGKILL after the client; %A swallows
 * the abrupt exit. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-041';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
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

    /* One connection, 16 requests, rebind before each from #2 — 15 back-to-back
     * migrations, far past the storm cap (default 8). DEADLINE_MS bounds a
     * regressed hang so the test still terminates. */
    $cmd = sprintf(
        'H3CLIENT_REQUEST_COUNT=16 H3CLIENT_MIGRATE_AFTER=1 H3CLIENT_DEADLINE_MS=6000 '
        . '%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);

    $t0  = hrtime(true);
    $out = shell_exec($cmd) ?? '';
    $elapsed = (hrtime(true) - $t0) / 1e9;

    echo "served_before_shed=", (substr_count($out, 'STATUS=200') >= 1 ? 1 : 0), "\n";

    /* Aggregate the shed counter across the per-reactor listener entries. */
    $shed = 0;
    foreach ($server->getHttp3Stats() as $st) {
        $shed += (int)($st['quic_migration_storm_shed'] ?? 0);
    }
    echo "storm_shed_ge1=", ($shed >= 1 ? 1 : 0), "\n";

    /* The shed is a prompt CONNECTION_CLOSE; a regressed wedge would burn the
     * full 6 s deadline. Generous bound to stay robust on a loaded CI box. */
    echo "no_hang=", ($elapsed < 4.0 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
?>
--EXPECTF--
%Aserved_before_shed=1
storm_shed_ge1=1
no_hang=1
%A
