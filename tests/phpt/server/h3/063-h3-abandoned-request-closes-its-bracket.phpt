--TEST--
HTTP/3 — a request whose peer left still closes its in-flight bracket
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
/* `active_requests` is a gauge the admission check reads, and HTTP/1 and HTTP/2
 * are the protocols that read it — HTTP/3 only feeds it. The dispatch raised it
 * through the connection and the dispose lowered it through the connection too,
 * but a peer that goes away has its streams' `conn` NULLed while their handlers
 * are still running, so the decrement was skipped and the gauge never came back
 * down. N abandoned HTTP/3 requests subtracted N from the budget for good, and
 * once the cap was reached the server answered 503 to everyone else.
 *
 * The same reach through `conn` dropped the request from telemetry, so a request
 * that ran to completion left no trace at all.
 *
 * The counters are taken at stream creation now. The assertion is the pairing:
 * the gauge is back where it started and the request is counted once. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = sys_get_temp_dir() . '/h3-abandon-' . getmypid();
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";

if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($dir, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($dir);
});

$port = tas_free_port_span(2);

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port + 1)
        ->addHttp3Listener('127.0.0.1', $port)
        ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
        ->setReadTimeout(10)->setWriteTimeout(10)
        ->setStatsEnabled(true)
);

$server->addHttpHandler(function ($req, $res) {
    /* Still working when the peer leaves — that is the whole window. */
    delay(1500);
    $res->setStatusCode(200)->setBody('ok');
});

spawn(function () use ($port, $server) {
    delay(400);

    $probe = __DIR__ . '/../../../h3client/h3probe.py';

    $run = static function (string $env) use ($probe, $port) {
        return trim((string) shell_exec(
            sprintf('%s python3 %s 127.0.0.1 %d /park 2>/dev/null',
                    $env, escapeshellarg($probe), $port)));
    };

    echo $run('H3PROBE_ABANDON_MS=400'), "\n";
    delay(4000);            /* well past the 1.5 s the handler works for */

    /* The connection stays open here, so the stream is released while the
     * handler still holds it — the release used to drop the request zval
     * without marking it gone, and the dispose then released it a second
     * time. On a debug build that aborts the process; on a release build it
     * is a write into a freed object bucket. */
    echo $run('H3PROBE_STOP_MS=400'), "\n";
    delay(4000);

    $t = $server->getStats()['totals'];
    echo 'active=', $t['active_requests'] ?? -1, "\n";
    echo 'total=',  $t['total_requests']  ?? -1, "\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
status=- bytes=- cl=- outcome=ABANDONED
status=- bytes=- cl=- outcome=STOP_SENDING
active=0
total=1
