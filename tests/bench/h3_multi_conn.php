<?php
/* H3 multi-connection bench. Spawns N parallel h3client processes
 * against the same server, each making M sequential requests over its
 * own QUIC connection. Reveals the recvmmsg/GSO win that single-conn
 * benches mask (single-conn can have ≤1 packet in flight per direction
 * at any time, so batching never grows). */

putenv('PHP_HTTP3_BENCH_FC=1');

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$CONNS  = (int)(getenv('BENCH_CONNS')  ?: 16);
$REQS   = (int)(getenv('BENCH_REQS')   ?: 500);   /* per connection */

$tmp  = __DIR__ . '/tmp-bench';
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
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('content-type', 'text/plain')->setBody('ok');
});

$client_bin = realpath(__DIR__ . '/../h3client/h3client');
fwrite(STDERR, sprintf("[bench] CONNS=%d REQS_PER_CONN=%d total=%d\n",
    $CONNS, $REQS, $CONNS * $REQS));

$client = spawn(function () use ($server, $port, $client_bin, $CONNS, $REQS) {
    usleep(150000);

    $stats0 = $server->getHttp3Stats()[0] ?? [];
    $r0 = (int)($stats0['h3_request_received'] ?? 0);
    $t0 = hrtime(true);

    /* Fire all clients in parallel via shell (background). */
    $cmd = sprintf(
        'seq 1 %d | xargs -P %d -I {} sh -c '
        . '"PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=%d H3CLIENT_QUIET=1 '
        . 'H3CLIENT_DEADLINE_MS=120000 %s 127.0.0.1 %d / GET 2>/dev/null"',
        $CONNS, $CONNS, $REQS, escapeshellarg($client_bin), $port);
    shell_exec($cmd);

    $t1 = hrtime(true);
    $stats1 = $server->getHttp3Stats()[0] ?? [];
    $r1 = (int)($stats1['h3_request_received'] ?? 0);

    $elapsed_ms = ($t1 - $t0) / 1e6;
    $delta_reqs = $r1 - $r0;

    printf("CONNS=%d\n", $CONNS);
    printf("REQS_PER_CONN=%d\n", $REQS);
    printf("EXPECTED=%d\n", $CONNS * $REQS);
    printf("SERVER_REQS=%d\n", $delta_reqs);
    printf("ELAPSED_MS=%.3f\n", $elapsed_ms);
    printf("RPS=%.1f\n", $delta_reqs / max(0.001, $elapsed_ms / 1000.0));
    printf("PACKETS_SENT=%d\n", (int)($stats1['quic_packets_sent'] ?? 0));
    printf("DGRAMS_RECV=%d\n", (int)($stats1['datagrams_received'] ?? 0));

    $server->stop();
});

$server->start();
await($client);
