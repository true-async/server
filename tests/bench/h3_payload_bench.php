<?php
/* H3 large-payload bench. Varies response body size to expose the
 * GSO and recvmmsg wins that small-response benches mask:
 * - Tiny responses (2 bytes "ok") — only 1-2 packets per response, 0-1
 *   GSO opportunities, 0-1 recv-batch opportunities.
 * - Larger responses force ngtcp2 to emit multiple packets per drain,
 *   which is exactly what GSO collapses into one sendmsg.
 *
 * Run:
 *   for sz in 64 1024 16384 102400; do
 *     BENCH_BODY=$sz BENCH_REQS=2000 \
 *     php -d extension_dir=$(pwd)/modules -d extension=true_async_server \
 *         tests/bench/h3_payload_bench.php
 *   done
 */

putenv('PHP_HTTP3_BENCH_FC=1');

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$N    = (int)(getenv('BENCH_REQS') ?: 2000);
$BODY = (int)(getenv('BENCH_BODY') ?: 1024);
$WARMUP = (int)(getenv('BENCH_WARMUP') ?: 50);

$tmp = __DIR__ . '/tmp-bench';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!is_file($cert)) {
    exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
        escapeshellarg($key), escapeshellarg($cert)));
}

$port = 30000 + (getmypid() % 30000);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);

/* Pre-build response body of requested size. */
$body = str_repeat('x', $BODY);
$server->addHttpHandler(function ($req, $res) use ($body) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody($body);
});

$client_bin = realpath(__DIR__ . '/../h3client/h3client');
fwrite(STDERR, sprintf("[bench] N=%d body=%d port=%d\n", $N, $BODY, $port));

$client = spawn(function () use ($server, $port, $client_bin, $N, $WARMUP, $BODY) {
    usleep(150000);

    if ($WARMUP > 0) {
        shell_exec(sprintf('PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=%d H3CLIENT_QUIET=1 '
            . 'H3CLIENT_DEADLINE_MS=60000 %s 127.0.0.1 %d / GET 2>/dev/null',
            $WARMUP, escapeshellarg($client_bin), $port));
    }

    $stats0 = $server->getHttp3Stats()[0] ?? [];
    $r0 = (int)($stats0['h3_request_received'] ?? 0);
    $p0 = (int)($stats0['quic_packets_sent'] ?? 0);
    $b0 = (int)($stats0['quic_bytes_sent']   ?? 0);

    $t0 = hrtime(true);
    $cmd = sprintf('PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=%d H3CLIENT_QUIET=1 '
        . 'H3CLIENT_DEADLINE_MS=120000 %s 127.0.0.1 %d / GET 2>&1',
        $N, escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';
    $t1 = hrtime(true);

    $completed = -1;
    if (preg_match('/^COMPLETED=(\d+)$/m', $out, $m)) $completed = (int)$m[1];

    $stats1 = $server->getHttp3Stats()[0] ?? [];
    $r1 = (int)($stats1['h3_request_received'] ?? 0);
    $p1 = (int)($stats1['quic_packets_sent'] ?? 0);
    $b1 = (int)($stats1['quic_bytes_sent']   ?? 0);

    $elapsed_ms = ($t1 - $t0) / 1e6;
    $reqs       = max(1, $r1 - $r0);
    $packets    = $p1 - $p0;
    $bytes_out  = $b1 - $b0;

    printf("BODY=%d  REQS=%d  COMPLETED=%d\n", $BODY, $N, $completed);
    printf("ELAPSED_MS=%.3f  RPS=%.1f  PER_REQ_US=%.2f\n",
        $elapsed_ms, $completed / max(0.001, $elapsed_ms / 1000),
        ($t1 - $t0) / 1e3 / max(1, $completed));
    printf("PACKETS_SENT=%d  PKTS_PER_REQ=%.2f\n",
        $packets, $packets / $reqs);
    printf("BYTES_SENT=%d  BYTES_PER_PKT=%.1f  THROUGHPUT_MBPS=%.1f\n",
        $bytes_out, $bytes_out / max(1, $packets),
        $bytes_out * 8 / 1e6 / max(0.001, $elapsed_ms / 1000));

    $server->stop();
});

$server->start();
await($client);
