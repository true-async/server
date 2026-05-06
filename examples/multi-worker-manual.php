<?php
/**
 * Multi-threaded TrueAsync HTTP server demo — MANUAL preforking layout.
 *
 *   php -d extension=./modules/true_async_server.so examples/multi-worker-manual.php
 *
 * For most use cases you want examples/multi-worker.php instead — that one
 * uses HttpServerConfig::setWorkers(N) and the parent's start() takes care
 * of fan-out, SO_REUSEPORT, and lifecycle automatically (issue #11).
 *
 * This file is kept as a reference for the cases where you need explicit
 * control over each worker's startup sequence (e.g. per-worker init that
 * runs BEFORE listeners come up: opcache warmup, DB pool bootstrap, custom
 * fixtures). The setWorkers() path doesn't expose a worker-init hook yet.
 *
 * Architecture
 *   - One PHP process, N worker threads (N = available_parallelism() or $WORKERS).
 *   - Each worker builds its own HttpServer and addListener()s the same port;
 *     SO_REUSEPORT lets the kernel load-balance accept() across them.
 *   - Handler closures + preloaded fixtures are captured by value and cloned
 *     into each worker thread by ThreadPool::submit() (transfer_obj).
 *
 * Endpoints (designed for a quick smoke test):
 *   GET  /              -> "ok"
 *   GET  /pid           -> worker pid + a per-worker counter
 *   GET  /baseline?a=1  -> sum of integer query values
 *   POST /baseline      -> sum + body integer
 *   GET  /json/<n>      -> n synthetic items as JSON
 *   GET  /stop          -> graceful shutdown of *this* worker
 */

declare(strict_types=1);

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;
use Async\ThreadPool;
use function Async\spawn;
use function Async\available_parallelism;

$port      = (int)(getenv('PORT') ?: 8080);
$tlsPort   = (int)(getenv('TLS_PORT') ?: 8443);
$h3Port    = (int)(getenv('H3_PORT') ?: $tlsPort);
$certPath  = getenv('TLS_CERT') ?: '/certs/server.crt';
$keyPath   = getenv('TLS_KEY')  ?: '/certs/server.key';
$tlsAvailable = is_readable($certPath) && is_readable($keyPath);

$envWorkers = (int)(getenv('WORKERS') ?: 0);
$autoCount  = available_parallelism();
$workers   = $envWorkers > 0 ? $envWorkers : $autoCount;

// Diagnostic: print every CPU-count source we can cheaply read, so when the
// auto-pick looks wrong we can see which source disagrees. available_parallelism
// (libuv) is cgroup-/affinity-aware; nproc and /proc/cpuinfo are not.
$nproc        = (int)trim((string)@shell_exec('nproc 2>/dev/null'));
$nprocAll     = (int)trim((string)@shell_exec('nproc --all 2>/dev/null'));
$cpuinfoCount = substr_count((string)@file_get_contents('/proc/cpuinfo'), "\nprocessor");
$cgroupMax    = trim((string)@file_get_contents('/sys/fs/cgroup/cpu.max'));

fprintf(
    STDERR,
    "[multi-worker] cpu sources: available_parallelism=%d nproc=%d nproc_all=%d cpuinfo=%d cgroup_cpu.max=%s\n",
    $autoCount, $nproc, $nprocAll, $cpuinfoCount, $cgroupMax !== '' ? $cgroupMax : 'n/a'
);

// Tiny synthetic dataset — no /data dependency.
$dataset = [];
for ($i = 0; $i < 64; $i++) {
    $dataset[] = [
        'id'       => $i,
        'name'     => "item-{$i}",
        'price'    => 1.0 + $i,
        'quantity' => $i + 1,
    ];
}
$datasetCount = count($dataset);

$workerMain = static function () use (
    $port, $tlsPort, $h3Port, $tlsAvailable, $certPath, $keyPath,
    $dataset, $datasetCount
): void {
    $handler = static function (HttpRequest $req, HttpResponse $res)
        use ($dataset, $datasetCount): void
    {
        $uri = $req->getUri();

        // Fast path: absolute-minimum handler, identical work shape to Swoole's
        // /pipeline test (one comparison, one setBody chain). No URL parsing,
        // no switch tree, no static counter.
        if ($uri === '/bare') {
            $res->setStatusCode(200)->setBody('ok');
            return;
        }

        static $count = 0;
        $count++;

        $qpos = strpos($uri, '?');
        $path = $qpos === false ? $uri : substr($uri, 0, $qpos);
        $query = [];
        if ($qpos !== false) {
            parse_str(substr($uri, $qpos + 1), $query);
        }

        if ($path === '/' || $path === '/pipeline') {
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setBody('ok');
            return;
        }

        if ($path === '/pid') {
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setBody("pid=" . getmypid() . " count={$count}\n");
            return;
        }

        // Diagnostic endpoints for isolating PHP-runtime cost from server cost.
        // Each one removes one layer of work compared to /json/3.
        if ($path === '/json3-static') {
            // No array ops, no json_encode — pure precomputed string.
            static $staticJson = '{"items":[{"id":0,"name":"item-0","price":1,"quantity":1},{"id":1,"name":"item-1","price":2,"quantity":2},{"id":2,"name":"item-2","price":3,"quantity":3}],"count":3}';
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'application/json')
                ->setBody($staticJson);
            return;
        }
        if ($path === '/json3-encode') {
            // json_encode of a tiny precomputed array — isolate encode cost.
            static $payload = ['items' => [['id'=>0,'name'=>'item-0','price'=>1,'quantity'=>1],['id'=>1,'name'=>'item-1','price'=>2,'quantity'=>2],['id'=>2,'name'=>'item-2','price'=>3,'quantity'=>3]], 'count' => 3];
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'application/json')
                ->setBody(json_encode($payload));
            return;
        }

        if ($path === '/opcache') {
            $s = function_exists('opcache_get_status') ? opcache_get_status(false) : null;
            $jit = $s['jit'] ?? [];
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'application/json')
                ->setBody(json_encode([
                    'opcache_enabled' => (bool)($s['opcache_enabled'] ?? false),
                    'cached_scripts'  => $s['opcache_statistics']['num_cached_scripts'] ?? 0,
                    'hits'            => $s['opcache_statistics']['hits'] ?? 0,
                    'misses'          => $s['opcache_statistics']['misses'] ?? 0,
                    'jit_enabled'     => (bool)($jit['enabled'] ?? false),
                    'jit_buffer_size' => $jit['buffer_size'] ?? 0,
                    'jit_buffer_used' => ($jit['buffer_size'] ?? 0) - ($jit['buffer_free'] ?? 0),
                    'jit_buffer_free' => $jit['buffer_free'] ?? 0,
                ]));
            return;
        }

        if ($path === '/baseline') {
            $sum = 0;
            foreach ($query as $v) { $sum += (int)$v; }
            if ($req->getMethod() === 'POST') {
                $sum += (int)$req->getBody();
            }
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')
                ->setBody((string)$sum);
            return;
        }

        if (preg_match('#^/json/(\d+)$#', $path, $m)) {
            $n = min((int)$m[1], $datasetCount);
            $items = array_slice($dataset, 0, $n);
            $res->setStatusCode(200)
                ->setHeader('Content-Type', 'application/json')
                ->setBody(json_encode(['items' => $items, 'count' => $n]));
            return;
        }

        $res->setStatusCode(404)
            ->setHeader('Content-Type', 'text/plain')
            ->setBody("not found: {$path}\n");
    };

    $config = (new HttpServerConfig())
        ->addListener('0.0.0.0', $port)
        ->setBacklog(2048)
        ->setReadTimeout(15)
        ->setWriteTimeout(15)
        ->setKeepAliveTimeout(60)
        ->setShutdownTimeout(5)
        ->setMaxBodySize(8 * 1024 * 1024);

    if ($tlsAvailable) {
        // HTTP/2 over TLS via ALPN, plus HTTP/1.1 fallback on the same port.
        $config->addListener('0.0.0.0', $tlsPort, true)
               ->setCertificate($certPath)
               ->setPrivateKey($keyPath);
        // HTTP/3 / QUIC on UDP — advertised via Alt-Svc by default.
        $config->addHttp3Listener('0.0.0.0', $h3Port);
    }

    $server = new HttpServer($config);
    $server->addHttpHandler($handler);
    $server->start();
};

spawn(function () use ($workerMain, $workers, $port, $tlsPort, $h3Port, $tlsAvailable): void {
    fprintf(
        STDERR,
        "[multi-worker] %d workers · http://0.0.0.0:%d%s · pid %d\n",
        $workers, $port,
        $tlsAvailable ? " · https/h2 :{$tlsPort} · h3/quic udp:{$h3Port}" : ' · (no TLS — set TLS_CERT/TLS_KEY to enable h2/h3)',
        getmypid()
    );

    $pool = new ThreadPool($workers);
    for ($i = 0; $i < $workers; $i++) {
        $pool->submit($workerMain);
    }

    while (true) {
        \Async\delay(1000);
    }
});
