--TEST--
Rate limit (#414): with setWorkers(2) every worker enforces the rules over HTTP/1 and HTTP/2, each with buckets of its own
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_SHARED_LISTEN_FD=1
--FILE--
<?php
/* The rules are copied to both worker clones, and each keeps its own
 * buckets: one request a minute per path, so of eight requests to a path
 * one or two are admitted, one per worker that took a connection, and the
 * rest are refused with 429. The workers' refusals add up in getStats().
 * getStats() also shows each worker that served a path admitted exactly one
 * request, which a bucket shared by the workers fails; that check bites only
 * when both workers take a connection, which the kernel does not promise but
 * which happened in every run here. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setStatsEnabled(true)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2));

$server->addRateLimit((new RateLimit(1, 60))->paths('/h1')->name('h1'));
$server->addRateLimit((new RateLimit(1, 60))->paths('/h2')->name('h2'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));

/* Eight requests, each on a connection of its own. */
function eight(int $port, string $path, string $curl_flags): array
{
    $seen = [];

    for ($i = 0; $i < 8; $i++) {
        $out = (string) shell_exec("curl -s -i --max-time 2 $curl_flags http://127.0.0.1:$port$path 2>&1");
        $status = preg_match('/^HTTP\/[\d.]+ (\d+)/m', $out, $m) ? $m[1] : 'none';

        if (preg_match('/^retry-after: (\d+)/mi', $out, $r)) {
            $status .= ' retry-after=' . rl_wait((int) $r[1]);
        }

        $seen[] = $status;
    }

    $counts = array_count_values($seen);
    ksort($counts);
    return $counts;
}

function verdict(string $label, array $counts): int
{
    $admitted = $counts['200'] ?? 0;
    unset($counts['200']);
    echo "$label admitted 1 or 2: ", ($admitted >= 1 && $admitted <= 2) ? 'yes' : "no ($admitted)",
         " refusals: ", json_encode(array_keys($counts)), "\n";
    return 8 - $admitted;
}

/* Requests served and refused per worker since $before, both from getStats(). */
function per_worker(HttpServer $server, array $before): array
{
    $now = [];

    foreach ($server->getStats()['workers'] as $id => $slot) {
        $now[$id] = [$slot['total_requests'], $slot['requests_rate_limited_total']];
    }

    $delta = [];

    foreach ($now as $id => [$served, $limited]) {
        $delta[$id] = [$served - ($before[$id][0] ?? 0), $limited - ($before[$id][1] ?? 0)];
    }

    return [$now, $delta];
}

/* Each worker that served the path admitted exactly one request: its own
 * bucket. A bucket shared by the workers admits one in all, so the second
 * worker to take traffic admits none. */
function each_worker_admits_one(string $label, array $delta): void
{
    $ok = true;

    foreach ($delta as [$served, $limited]) {
        if ($served === 0) {
            continue;
        }

        $ok = $ok && $served - $limited === 1;
    }

    echo "$label: every worker that served admitted one: ", $ok ? 'yes' : 'no ' . json_encode($delta), "\n";
}

spawn(function () use ($server, $port) {
    delay(400);
    [$before] = per_worker($server, []);
    $refused  = verdict('HTTP/1', eight($port, '/h1', '--http1.1'));
    [$before, $delta] = per_worker($server, $before);
    each_worker_admits_one('HTTP/1', $delta);
    $refused += verdict('HTTP/2', eight($port, '/h2', '--http2-prior-knowledge'));
    [, $delta] = per_worker($server, $before);
    each_worker_admits_one('HTTP/2', $delta);

    $totals = $server->getStats()['totals'];
    echo "getStats rate_limited matches: ",
         $totals['requests_rate_limited_total'] === $refused ? 'yes' : "no ({$totals['requests_rate_limited_total']} vs $refused)", "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%AHTTP/1 admitted 1 or 2: yes refusals: ["429 retry-after=60"]
HTTP/1: every worker that served admitted one: yes
HTTP/2 admitted 1 or 2: yes refusals: ["429 retry-after=60"]
HTTP/2: every worker that served admitted one: yes
getStats rate_limited matches: yes
%Adone
