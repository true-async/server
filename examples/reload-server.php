<?php
/**
 * Hot reload — rotate the worker pool to pick up new code without dropping
 * the listen sockets or in-flight requests (issue #93).
 *
 * Run (reload needs the worker pool, so setWorkers() > 1):
 *   php examples/reload-server.php            # prints the pool-parent pid
 *   PORT=9000 php examples/reload-server.php
 *
 * Try it — in another terminal:
 *   curl 127.0.0.1:8080/                      # version=v1 boot=<id>
 *   echo v2 > examples/app-version.txt        # "change the app"
 *   curl 127.0.0.1:8080/                      # STILL v1 — code is loaded at boot
 *   kill -HUP <pool-parent-pid>               # rotate the pool
 *   curl 127.0.0.1:8080/                      # now v2, fresh boot id, zero downtime
 *
 * (Workers are threads in one process, so they share a pid — the per-boot
 *  `boot` id below is what changes when a worker is replaced by a reload.)
 *
 * The bootloader runs once per worker before its request loop — the place to
 * `require` your app / warm a DB pool / prime opcache. A reload re-runs it with
 * the new code on replacement workers, so edits apply atomically per rotation.
 *
 * Triggers:
 *   - enableReloadOnSignal()  — prod: SIGHUP rotates the pool (used below).
 *   - enableHotReload([dirs]) — dev: a filesystem watcher rotates on file change.
 *   - $server->reload()       — programmatic, from any coroutine.
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpRequest;
use TrueAsync\HttpResponse;

$versionFile = __DIR__ . '/app-version.txt';
if (!is_file($versionFile)) {
    file_put_contents($versionFile, 'v1');
}

$config = (new HttpServerConfig())
    ->addListener('0.0.0.0', (int)(getenv('PORT') ?: 8080))
    ->setWorkers(2)
    ->setBootloader(function () use ($versionFile): void {
        // Loaded ONCE per worker at boot — a reload re-runs this with new code.
        $GLOBALS['APP_VERSION'] = trim((string) @file_get_contents($versionFile)) ?: 'v1';
        $GLOBALS['BOOT_ID']     = bin2hex(random_bytes(3));   // new on every (re)boot
    })
    ->enableReloadOnSignal();          // SIGHUP → HttpServer::reload()

    // Dev alternative — watch the directory and reload on any *.php change:
    // ->enableHotReload([__DIR__]);

$server = new HttpServer($config);

$server->addHttpHandler(function (HttpRequest $req, HttpResponse $res) {
    $res->setBody(sprintf(
        "version=%s boot=%s\n",
        $GLOBALS['APP_VERSION'] ?? '?',
        $GLOBALS['BOOT_ID'] ?? '?'
    ));
});

fprintf(STDERR, "[reload] :%d pool-parent pid=%d — reload with: kill -HUP %d\n",
    (int)(getenv('PORT') ?: 8080), getmypid(), getmypid());

$server->start();
