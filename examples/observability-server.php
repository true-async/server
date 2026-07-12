<?php

declare(strict_types=1);

/*
 * Observability: getStats() -> Prometheus -> Grafana, plus a JSON access log.
 *
 * The server has no embedded exporter. getStats() returns a plain PHP array and
 * $render below turns it into the Prometheus text format — swap that for
 * OpenMetrics or StatsD without touching the server.
 *
 *   php examples/observability-server.php
 *   curl -s localhost:8080/metrics
 *
 *   docker compose -f examples/docker/observability/docker-compose.yml up -d
 *   open http://localhost:3001          # Grafana, no login, dashboard loaded
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;

$port    = (int) (getenv('PORT') ?: 8080);
$workers = (int) (getenv('WORKERS') ?: 4);
$logPath = getenv('ACCESS_LOG') ?: (sys_get_temp_dir() . '/tas-access.log');

/*
 * A closure, not a `function render(...)`. Under setWorkers(N) the handler is
 * copied into a worker thread, which has its own function table — a named
 * function declared here does not exist there and calling it aborts the worker.
 * Closures are copied, so reach them through use() (or declare them in a
 * bootloader).
 *
 * A *_total is monotonic and becomes a Prometheus `counter`; the rest are
 * point-in-time readings and become `gauge`s. That split is not cosmetic: the
 * server inherits a retiring worker's totals but not its gauges, so across a
 * reload() tas_requests_total keeps climbing while tas_conns_active_h1 falls
 * back to what is really open.
 */
$render = function (array $stats): string {
    /* Prometheus wants the _total as a SUFFIX; the server spells this one the
     * other way round. */
    $rename = ['total_requests' => 'requests_total'];

    $out = [];

    foreach ($stats['totals'] as $name => $value) {
        $name   = $rename[$name] ?? $name;
        $metric = 'tas_' . $name;
        $out[]  = '# TYPE ' . $metric . ' '
                . (str_ends_with($name, '_total') ? 'counter' : 'gauge');
        $out[]  = $metric . ' ' . $value;

        /* One series per worker: sum() them back up, or spot the single worker
         * that has gone quiet. */
        foreach ($stats['workers'] as $id => $counters) {
            $raw = array_search($name, $rename, true) ?: $name;
            if (isset($counters[$raw])) {
                $out[] = $metric . '{worker="' . $id . '"} ' . $counters[$raw];
            }
        }
    }

    $out[] = '# TYPE tas_workers gauge';
    $out[] = 'tas_workers ' . count($stats['workers']);

    return implode("\n", $out) . "\n";
};

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', $port)
    ->setWorkers($workers)
    ->setStatsEnabled(true)          /* opt-in: no counters, no cost, without it */
    ->setLogSinks([
        /* 'file', not 'stream': each worker reopens the path itself, whereas a
         * parent-opened stream resource cannot cross into a worker thread.
         * 'access' = one record per request, in OTel semantic conventions. */
        [
            'type'     => 'file',
            'path'     => $logPath,
            'format'   => 'json',
            'category' => 'access',
            'level'    => LogSeverity::INFO,
        ],
        [
            'type'     => 'stderr',
            'format'   => 'pretty',
            'category' => 'app',
            'level'    => LogSeverity::INFO,
        ],
    ]);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) use ($server, $render) {
    switch ($req->getPath()) {
        case '/metrics':
            /* Prometheus scrapes this. Content type must be text/plain. */
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain; version=0.0.4')
                ->setBody($render($server->getStats()))
                ->end();
            return;

        case '/slow':
            /* Something to make the latency panels move. */
            Async\delay(random_int(20, 300));
            $res->setStatusCode(200)->setBody("slow\n")->end();
            return;

        case '/boom':
            /* Feeds responses_5xx_total. */
            $res->setStatusCode(500)->setBody("boom\n")->end();
            return;

        case '/missing':
            $res->setStatusCode(404)->setBody("not found\n")->end();
            return;

        default:
            $res->setStatusCode(200)
                ->setBody("hello from " . $req->getRemoteAddress() . "\n")
                ->end();
    }
});

fwrite(STDERR, sprintf(
    "listening on :%d with %d workers\n  metrics    http://localhost:%d/metrics\n  access log %s\n",
    $port, $workers, $port, $logPath
));

$server->start();
