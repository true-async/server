<?php

declare(strict_types=1);

/*
 * Observability: getStats() -> Prometheus -> Grafana, plus a JSON access log.
 *
 * The server does not speak Prometheus itself, and deliberately so — an
 * embedded exporter would drag a scrape format, a text encoder and a config
 * surface into the extension. getStats() hands you a plain PHP array; the
 * twenty lines of render_prometheus() below turn it into the text format. Swap
 * them for OpenMetrics, StatsD or a JSON blob without touching the server.
 *
 * Run it:
 *   php examples/observability-server.php
 *   curl -s localhost:8080/metrics
 *
 * With Prometheus + Grafana (see examples/docker/observability/):
 *   docker compose -f examples/docker/observability/docker-compose.yml up -d
 *   open http://localhost:3001    (anonymous admin, dashboard pre-loaded)
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;

$port    = (int) (getenv('PORT') ?: 8080);
$workers = (int) (getenv('WORKERS') ?: 4);
$logPath = getenv('ACCESS_LOG') ?: (sys_get_temp_dir() . '/tas-access.log');

/*
 * A CLOSURE, not a named function.
 *
 * Under setWorkers(N) the handler is copied into a worker thread, and that
 * thread has its own function table: a `function render_prometheus()` declared
 * here does not exist there, and calling it aborts the worker. Closures do get
 * copied — so anything the handler calls must reach it through use(), or be
 * declared in a bootloader.
 *
 * The rendering itself: a counter's NAME decides its Prometheus type. A *_total
 * is monotonic and only grows, so it is a `counter`; everything else here is a
 * point-in-time reading, so it is a `gauge`. Getting this backwards is the
 * classic exporter bug — rate() over a gauge, or a counter a scraper thinks
 * reset.
 *
 * The server draws the same distinction internally, which is what makes the
 * numbers trustworthy across a reload(): monotonic totals are inherited from a
 * retiring worker, so tas_requests_total keeps climbing, while gauges are summed
 * over live workers only, so tas_conns_active_h1 drops to what is really open
 * instead of stranding a dead worker's last reading.
 */
$render = function (array $stats): string {
    /* Prometheus names a counter with a _total SUFFIX. Most of the server's
     * counters already read that way; `total_requests` does not, so rename it
     * rather than emit a counter the convention says is a gauge. */
    $rename = ['total_requests' => 'requests_total'];

    $out = [];

    foreach ($stats['totals'] as $name => $value) {
        $name   = $rename[$name] ?? $name;
        $metric = 'tas_' . $name;
        $out[]  = '# TYPE ' . $metric . ' '
                . (str_ends_with($name, '_total') ? 'counter' : 'gauge');
        $out[]  = $metric . ' ' . $value;

        /* Same metric, one series per worker: sum() them back up, or spot the
         * single worker that is misbehaving. */
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
        /* 'file' (not 'stream') is the sink that works under a worker pool:
         * each worker reopens the path itself, whereas a parent-opened stream
         * resource cannot cross into a worker thread.
         *
         * category 'access' = one structured record per completed request.
         * The attributes use OpenTelemetry HTTP semantic conventions, so a
         * collector reads them without a custom mapping. */
        [
            'type'     => 'file',
            'path'     => $logPath,
            'format'   => 'json',
            'category' => 'access',
            'level'    => LogSeverity::INFO,
        ],
        /* Server diagnostics to the console, human-readable. */
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
