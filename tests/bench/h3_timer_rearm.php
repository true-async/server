<?php
/* Bench harness for the H3 hot path on a single QUIC connection.
 *
 * Boots an H3 server, fires N sequential requests over one QUIC
 * connection via h3client (H3CLIENT_REQUEST_COUNT mode), measures wall
 * time + server-side user CPU and prints per-request latency / RPS.
 *
 * Originally written to A/B-test the timer rearm optimization
 * (zend_async_timer_rearm_fn / ZEND_ASYNC_TIMER_F_MULTISHOT). Kept as
 * a regression harness for any future H3 hot-path work.
 *
 * Run:
 *   php -d extension=modules/http_server.so tests/bench/h3_timer_rearm.php
 *   BENCH_REQS=20000 /usr/bin/time php -d extension=modules/http_server.so \
 *       tests/bench/h3_timer_rearm.php
 */

putenv('PHP_HTTP3_BENCH_FC=1');

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$N = (int)(getenv('BENCH_REQS') ?: 5000);
$WARMUP = (int)(getenv('BENCH_WARMUP') ?: 50);

$tmp = __DIR__ . '/tmp-bench';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!is_file($cert)) {
    $rc = 0;
    exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
        escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
    if ($rc !== 0) { fwrite(STDERR, "cert gen failed\n"); exit(1); }
}

$port = 30000 + (getmypid() % 30000);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody('ok');
});

$client_bin = realpath(__DIR__ . '/../h3client/h3client');
if (!$client_bin || !is_executable($client_bin)) {
    fwrite(STDERR, "h3client not built at $client_bin\n"); exit(1);
}

fwrite(STDERR, sprintf("[bench] N=%d warmup=%d port=%d\n", $N, $WARMUP, $port));

$client = spawn(function () use ($server, $port, $client_bin, $N, $WARMUP) {
    usleep(120000);

    /* Warmup */
    if ($WARMUP > 0) {
        $cmd = sprintf('PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=%d H3CLIENT_QUIET=1 '
            . 'H3CLIENT_DEADLINE_MS=60000 %s 127.0.0.1 %d / GET 2>/dev/null',
            $WARMUP, escapeshellarg($client_bin), $port);
        shell_exec($cmd);
    }

    $stats0 = $server->getHttp3Stats()[0] ?? [];
    $r0 = (int)($stats0['h3_request_received'] ?? 0);

    $t0 = hrtime(true);
    $cmd = sprintf('PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=%d H3CLIENT_QUIET=1 '
        . 'H3CLIENT_DEADLINE_MS=120000 %s 127.0.0.1 %d / GET 2>&1',
        $N, escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';
    $t1 = hrtime(true);

    $completed = -1;
    if (preg_match('/^COMPLETED=(\d+)$/m', $out, $m)) $completed = (int)$m[1];
    if ($completed < 0) {
        fwrite(STDERR, "[bench] h3client output (last 500 chars):\n");
        fwrite(STDERR, substr($out, max(0, strlen($out) - 500)) . "\n");
    }

    $stats1 = $server->getHttp3Stats()[0] ?? [];
    $r1 = (int)($stats1['h3_request_received'] ?? 0);

    $elapsed_ns = $t1 - $t0;
    $elapsed_ms = $elapsed_ns / 1e6;
    $delta_reqs = $r1 - $r0;

    printf("REQUESTS=%d\n", $N);
    printf("COMPLETED=%d\n", $completed);
    printf("SERVER_REQS=%d\n", $delta_reqs);
    printf("ELAPSED_MS=%.3f\n", $elapsed_ms);
    printf("PER_REQ_US=%.3f\n", ($elapsed_ns / 1e3) / max(1, $completed));
    printf("RPS=%.1f\n", $completed / max(0.001, $elapsed_ms / 1000.0));

    $server->stop();
});

$server->start();
await($client);
