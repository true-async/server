<?php
/* H3 multi-conn × payload bench. The combination exercises both
 * recvmmsg batching (multi-conn) and GSO aggregation (large body). */

putenv('PHP_HTTP3_BENCH_FC=1');

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$CONNS = (int)(getenv('BENCH_CONNS') ?: 16);
$REQS  = (int)(getenv('BENCH_REQS')  ?: 200);
$BODY  = (int)(getenv('BENCH_BODY')  ?: 8192);

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
$body = str_repeat('x', $BODY);
$server->addHttpHandler(function ($req, $res) use ($body) {
    $res->setStatusCode(200)->setHeader('content-type', 'text/plain')->setBody($body);
});

$client_bin = realpath(__DIR__ . '/../h3client/h3client');
fwrite(STDERR, sprintf("[bench] CONNS=%d REQS_PER_CONN=%d body=%d port=%d\n",
    $CONNS, $REQS, $BODY, $port));

$client = spawn(function () use ($server, $port, $client_bin, $CONNS, $REQS) {
    usleep(150000);
    $stats0 = $server->getHttp3Stats()[0] ?? [];
    $r0 = (int)($stats0['h3_request_received'] ?? 0);
    $p0 = (int)($stats0['quic_packets_sent'] ?? 0);
    $b0 = (int)($stats0['quic_bytes_sent']   ?? 0);
    $d0 = (int)($stats0['datagrams_received'] ?? 0);

    $t0 = hrtime(true);
    $cmd = sprintf(
        'seq 1 %d | xargs -P %d -I {} sh -c '
        . '"PHP_HTTP3_BENCH_FC=1 H3CLIENT_REQUEST_COUNT=%d H3CLIENT_QUIET=1 '
        . 'H3CLIENT_DEADLINE_MS=120000 %s 127.0.0.1 %d / GET 2>/dev/null"',
        $CONNS, $CONNS, $REQS, escapeshellarg($client_bin), $port);
    shell_exec($cmd);
    $t1 = hrtime(true);

    $stats1 = $server->getHttp3Stats()[0] ?? [];
    $reqs    = (int)($stats1['h3_request_received'] ?? 0) - $r0;
    $packets = (int)($stats1['quic_packets_sent'] ?? 0) - $p0;
    $bytes   = (int)($stats1['quic_bytes_sent']   ?? 0) - $b0;
    $dgrams  = (int)($stats1['datagrams_received'] ?? 0) - $d0;

    $elapsed = ($t1 - $t0) / 1e6;
    printf("CONNS=%d REQS_PER=%d EXPECTED=%d SERVER_REQS=%d\n",
        $CONNS, $REQS, $CONNS * $REQS, $reqs);
    printf("ELAPSED_MS=%.3f RPS=%.1f\n",
        $elapsed, $reqs / max(0.001, $elapsed / 1000));
    printf("PACKETS=%d  PKTS_PER_REQ=%.2f  BYTES_PER_PKT=%.1f\n",
        $packets, $packets / max(1, $reqs), $bytes / max(1, $packets));
    printf("DGRAMS_RECV=%d\n", $dgrams);
    printf("THROUGHPUT_MBPS=%.1f\n", $bytes * 8 / 1e6 / max(0.001, $elapsed / 1000));
    $server->stop();
});

$server->start();
await($client);
