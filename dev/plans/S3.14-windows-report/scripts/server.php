<?php
/* S3.14 burst server: one TCP listener on 127.0.0.1 with backlog 1024, a handler
 * that answers "ok", stats on. The harness (run.py) talks to it through files in
 * a control directory:
 *   ready       written once every worker has a stats slot, plus a settle delay;
 *   done        written by the harness after the burst; holds the number of
 *               requests the clients got answered;
 *   stats.json  written here: each worker's total_requests and the totals,
 *               read once the totals reach that number (or after a timeout).
 *
 * Usage: php server.php <port> <workers> <uncapped|capped> <control dir> */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

const BACKLOG = 1024;
const CAP = 100000;
const SETTLE_US = 300000;
const POLL_US = 10000;
const STATS_WAIT_US = 5000000;
const READY_WAIT_US = 20000000;

/* The harness polls for these files and reads them at once: write aside, then rename. */
function publish(string $path, string $data): void
{
    file_put_contents("$path.tmp", $data);
    rename("$path.tmp", $path);
}

[, $port, $workers, $mode, $ctl] = $argv;
$port = (int) $port;
$workers = (int) $workers;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBacklog(BACKLOG)
    ->setWorkers($workers)
    ->setStatsEnabled(true);

if ($mode === 'capped') {
    $config->setMaxConnections(CAP);
} elseif ($mode !== 'uncapped') {
    fwrite(STDERR, "unknown mode $mode\n");
    exit(2);
}

/* CoDel must be off in both configurations: the default is 0, and the
 * CODEL_TARGET_MS environment variable would override it. */
if ($config->getBackpressureTargetMs() !== 0 || getenv('CODEL_TARGET_MS') !== false) {
    fwrite(STDERR, "CoDel is on\n");
    exit(2);
}

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok')->end();
});

spawn(function () use ($server, $workers, $ctl, $config) {
    for ($waited = 0; count($server->getStats()['workers']) < $workers; $waited += POLL_US) {
        if ($waited >= READY_WAIT_US) {
            fwrite(STDERR, "workers did not come up\n");
            $server->stop();
            return;
        }

        usleep(POLL_US);
    }

    usleep(SETTLE_US);
    publish("$ctl/ready", json_encode([
        'max_connections' => $config->getMaxConnections(),
        'codel_ms' => $config->getBackpressureTargetMs(),
        'backlog' => $config->getBacklog(),
        'workers' => $config->getWorkers(),
    ]));

    while (!file_exists("$ctl/done")) {
        usleep(POLL_US);
    }

    $expected = (int) file_get_contents("$ctl/done");
    $stats = $server->getStats();

    for ($waited = 0; $stats['totals']['total_requests'] < $expected && $waited < STATS_WAIT_US; $waited += POLL_US) {
        usleep(POLL_US);
        $stats = $server->getStats();
    }

    $per_worker = [];
    foreach ($stats['workers'] as $id => $w) {
        $per_worker[(string) $id] = $w['total_requests'] ?? null;
    }

    publish("$ctl/stats.json", json_encode([
        'workers' => $per_worker,
        'total_requests' => $stats['totals']['total_requests'],
        'total_connections' => $stats['totals']['total_connections'] ?? null,
    ]));

    $server->stop();
});

$server->start();
