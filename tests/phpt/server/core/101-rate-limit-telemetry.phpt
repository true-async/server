--TEST--
Rate limit (#414): refusals and evictions are counted in getTelemetry(), getStats() and per rule, a 429 counts as a 4xx, and resetTelemetry() clears them
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* 3000 forwarded clients through a trusted proxy against a table of 1024
 * buckets: new clients must take buckets from others, which counts
 * evictions, and no client is refused, since each starts with a full bucket.
 * Then one client is refused twice. */

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
    ->setTrustedProxies(['127.0.0.1'])
    ->setStatsEnabled(true)
    ->setReadTimeout(30)
    ->setWriteTimeout(30));

$server->addRateLimit((new RateLimit(1, 60))->maxKeys(1024)->name('per_client'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('ok'));

spawn(function () use ($server, $port) {
    delay(50);
    $fp = rl_connect($port);
    $statuses = [];

    for ($i = 0; $i < 3000; $i++) {
        $client = sprintf('10.%d.%d.%d', ($i >> 16) & 255, ($i >> 8) & 255, $i & 255);
        $statuses[] = rl_get($fp, '/', ['X-Forwarded-For' => $client])['status'] ?? 0;
    }

    echo "distinct clients: ", json_encode(array_count_values($statuses)), "\n";

    $statuses = [];
    for ($i = 0; $i < 3; $i++) {
        $statuses[] = rl_get($fp, '/', ['X-Forwarded-For' => '203.0.113.77'])['status'] ?? 0;
    }
    echo "one client three times: ", implode(' ', $statuses), "\n";
    fclose($fp);

    $t = $server->getTelemetry();
    $evictions = $t['requests_rate_limit_evictions_total'];
    echo "limited=", $t['requests_rate_limited_total'], "\n";
    echo "evictions at least 3000 - 1024: ", $evictions >= 3000 - 1024 ? 'yes' : "no ($evictions)", "\n";
    echo "rule matches totals: ", ($t['rate_limits']['per_client']['limited'] === 2
        && $t['rate_limits']['per_client']['evictions'] === $evictions) ? 'yes' : 'no', "\n";

    $totals = $server->getStats()['totals'];
    echo "stats: limited=", $totals['requests_rate_limited_total'],
         " evictions match=", $totals['requests_rate_limit_evictions_total'] === $evictions ? 'yes' : 'no',
         " 4xx=", $totals['responses_4xx_total'], " total=", $totals['total_requests'], "\n";

    $server->resetTelemetry();
    $t = $server->getTelemetry();
    echo "after reset: limited=", $t['requests_rate_limited_total'],
         " evictions=", $t['requests_rate_limit_evictions_total'],
         " rule=", json_encode($t['rate_limits']['per_client']), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
--EXPECTF--
distinct clients: {"200":3000}
one client three times: 200 429 429
limited=2
evictions at least 3000 - 1024: yes
rule matches totals: yes
stats: limited=2 evictions match=yes 4xx=2 total=3003
after reset: limited=0 evictions=0 rule={"limited":0,"evictions":0}
done
