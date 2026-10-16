--TEST--
Rate limit (#414): under the reactor pool the worker enforces the rules it loaded, gRPC included
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h3/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* The rules are copied to the worker clones with the static mounts, and each
 * worker keeps buckets of its own, so with two workers a client may be
 * admitted up to twice per window: of six requests one or two pass and the
 * rest are refused with 429, or with grpc-status 8 for a gRPC call. The
 * workers' refusals add up in getStats(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/../h3/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-100';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
$frame = "$tmp/frame.bin";
file_put_contents($frame, "\x00" . pack('N', 4) . 'ping');
register_shutdown_function(function () use ($tmp, $cert, $key, $frame) {
    @unlink($cert); @unlink($key); @unlink($frame); @rmdir($tmp);
});

$port = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key)
    ->setStatsEnabled(true)
    ->setWorkers(2));

$server->addRateLimit((new RateLimit(1, 60))->paths('/h3')->name('h3'));
$server->addRateLimit((new RateLimit(1, 60))->paths('/svc.Greeter')->name('grpc'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));
$server->addGrpcHandler(function ($req, $res) {
    $req->awaitBody();
    $res->writeMessage('pong');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

function h3(string $bin, int $port, string $path, string $method = 'GET', ?string $body = null, ?string $header = null): string
{
    $env = 'H3CLIENT_DEADLINE_MS=4000 H3CLIENT_PRINT_HEADERS=1 '
         . ($header !== null ? 'H3CLIENT_HEADER=' . escapeshellarg($header) . ' ' : '');
    $out = shell_exec(sprintf('%s%s 127.0.0.1 %d %s %s %s 2>&1 >/dev/null', $env,
        escapeshellarg($bin), $port, escapeshellarg($path), $method,
        $body !== null ? escapeshellarg($body) : '')) ?? '';

    preg_match('/^STATUS=(\d+)$/m', $out, $status);
    preg_match('/^HDR grpc-status: (\d+)$/m', $out, $grpc);
    preg_match('/^HDR retry-after: (\d+)$/m', $out, $retry);

    return ($status[1] ?? '?') . (isset($grpc[1]) ? " grpc-status=$grpc[1]" : '')
         . (isset($retry[1]) ? " retry-after=$retry[1]" : '');
}

spawn(function () use ($server, $port, $client_bin, $frame) {
    delay(600);
    $seen = [];

    for ($i = 0; $i < 6; $i++) {
        $seen[] = h3($client_bin, $port, '/h3');
    }

    $counts = array_count_values($seen);
    ksort($counts);
    $admitted = $counts['200'] ?? 0;
    echo "GET admitted 1 or 2: ", ($admitted >= 1 && $admitted <= 2) ? 'yes' : "no ($admitted)", "\n";
    echo "GET refused: ", $counts['429 retry-after=60'] ?? 0, " of ", 6 - $admitted, "\n";

    $grpc = [];

    for ($i = 0; $i < 6; $i++) {
        $grpc[] = h3($client_bin, $port, '/svc.Greeter/Ping', 'POST', $frame, 'content-type: application/grpc');
    }

    $counts = array_count_values($grpc);
    $ok = $counts['200 grpc-status=0'] ?? 0;
    echo "gRPC admitted 1 or 2: ", ($ok >= 1 && $ok <= 2) ? 'yes' : "no ($ok)", "\n";
    echo "gRPC refused: ", $counts['200 grpc-status=8'] ?? 0, " of ", 6 - $ok, "\n";

    $refused = 12 - ($admitted + $ok);
    delay(100);
    $totals = $server->getStats()['totals'];
    echo "rate_limited_total matches: ", $totals['requests_rate_limited_total'] === $refused ? 'yes' : 'no', "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%AGET admitted 1 or 2: yes
GET refused: %d of %d
gRPC admitted 1 or 2: yes
gRPC refused: %d of %d
rate_limited_total matches: yes
%Adone
