--TEST--
HttpServer: admission reject on in-flight cap — H2 gets RST_STREAM REFUSED_STREAM
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['h2load' => true]);
?>
--FILE--
<?php
/* Reproducer for PLAN_HTTP2 §Open follow-ups #3 H2 path.
 *
 * setMaxInflightRequests(2) + a 200 ms sleep in the handler.
 * h2load opens 1 connection with 20 concurrent streams (m=20 n=20).
 * The first 2 streams reach the handler and park; streams 3..20
 * hit cb_on_begin_headers while active_requests >= 2 and get
 * RST_STREAM(REFUSED_STREAM) immediately.
 *
 * The connection stays up through all resets (that's the whole point
 * of per-stream admission, vs. closing the connection). h2load
 * reports failed=18 and the server's h2_streams_refused_total
 * increments to 18.
 *
 * Verifies:
 *  - ≥1 stream is REFUSED (admission actually fires)
 *  - telemetry counter matches h2load's failed count
 *  - server still answers a fresh H2 request after the burst. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19980 + getmypid() % 10;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxConnections(32)
    ->setMaxInflightRequests(2)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttp2Handler(function ($req, $res) {
    delay(200);
    $res->setStatusCode(200)->setBody('ok');
});
/* Fallback for H1 post-burst probe. */
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* One conn, 20 concurrent streams. Expected: 2 succeed, 18 refused. */
    $cmd = sprintf(
        'h2load -n 20 -c 1 -m 20 -t 1 --connection-active-timeout=5 '
        . 'http://127.0.0.1:%d/ 2>&1',
        $port
    );
    $out = shell_exec($cmd);
    /* Parse: "20 total, ... done, X succeeded, Y failed" */
    $succeeded = 0; $failed = 0;
    if (preg_match('/(\d+) succeeded, (\d+) failed/', $out, $m)) {
        $succeeded = (int)$m[1];
        $failed    = (int)$m[2];
    }
    echo "succeeded_ge_1=", ($succeeded >= 1 ? 'yes' : 'no'), "\n";
    echo "failed_ge_1=",    ($failed    >= 1 ? 'yes' : 'no'), "\n";

    /* Give the server a beat to drain its in-flight slots. */
    delay(300);

    $tel = $server->getTelemetry();
    echo "h2_streams_refused_ge_1=",
         ($tel['h2_streams_refused_total'] >= 1 ? 'yes' : 'no'), "\n";
    echo "refused_matches_failed=",
         ($tel['h2_streams_refused_total'] === $failed ? 'yes' : 'no'), "\n";

    /* Post-burst H1 probe — server must still be responsive. */
    $probe = shell_exec(sprintf(
        'curl -s -o /dev/null -w "%%{http_code}" --max-time 2 http://127.0.0.1:%d/ping',
        $port));
    echo "post_burst_h1=$probe\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
succeeded_ge_1=yes
failed_ge_1=yes
h2_streams_refused_ge_1=yes
refused_matches_failed=yes
post_burst_h1=200
done
