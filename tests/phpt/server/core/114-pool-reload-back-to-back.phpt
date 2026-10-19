--TEST--
HttpServer: back-to-back reload() — a worker that starts during the RELOAD broadcast still retires
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
exec('curl --version 2>&1', $out, $rc);
if ($rc !== 0) die('skip curl CLI not available');
?>
--FILE--
<?php
/* reload() publishes RELOAD and rings every worker that has joined the pool
 * control channel. A fresh worker from the previous reload() may still be
 * starting: it read the command as NONE, and joined the channel only after the
 * broadcast. Nothing rang it, it served on, and reload() waited for its exit
 * forever. Reloading again at once keeps a fresh cohort starting during every
 * broadcast; on 2026-10-19 (Linux, 16 CPUs, debug ZTS) the test hung on 9 of 20
 * runs without the fix and on 0 of 50 with it.
 *
 * The watchdog turns a hang into a failure line; graceful_shutdown() cancels
 * it once, so it waits in a loop until start() has returned. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

const WORKERS = 4;
const RELOADS = 40;
const WATCHDOG_S = 30;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(WORKERS)
    ->setBootloader(static function (): void {});

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('pong');
});

$phase = 'start';
$stopped = false;

spawn(function () use (&$phase, &$stopped) {
    $deadline = time() + WATCHDOG_S;

    while (!$stopped && time() < $deadline) {
        try {
            delay(200);
        } catch (\Async\AsyncCancellation) {
        }
    }

    /* exit() waits for the stuck start() like everything else; a signal does not. */
    if (!$stopped) {
        echo "watchdog: hung in ", $phase, "\n";
        shell_exec('kill -KILL ' . getmypid());
    }
});

spawn(function () use ($server, $port, &$phase) {
    $up = '';
    for ($i = 0; $i < 50 && $up !== 'pong'; $i++) {
        usleep(100000);
        $up = (string) shell_exec(sprintf('curl -s --max-time 2 http://127.0.0.1:%d/', $port));
    }
    echo "up=", $up, "\n";

    $reloaded = 0;
    for ($i = 0; $i < RELOADS; $i++) {
        $phase = "reload $i";
        if ($server->reload()) {
            $reloaded++;
        }
    }
    echo "reloaded=", $reloaded, "\n";

    $phase = 'shutdown';
    \Async\graceful_shutdown();
});

$server->start();
$stopped = true;
echo "stopped=clean\n";
?>
--EXPECTF--
%Aup=pong
%Areloaded=40
%Astopped=clean%A
