--TEST--
HttpServer: reload() keeps every retired worker's completion, so start() returns after graceful_shutdown()
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
exec('curl --version 2>&1', $out, $rc);
if ($rc !== 0) die('skip curl CLI not available');
require __DIR__ . '/../../websocket/_ws_spread_skipif.inc';
ws_spread_skipif();
?>
--FILE--
<?php
/* php-async's ThreadPool::reload() returns once every old worker has posted its
 * exit token, and it can collect buffered tokens without a reactor tick, so the
 * completions of the tasks those workers ran may still be on their way to the
 * parent. reload() used to release those futures at once, the completion was
 * dropped with them, pending never reached zero and start() waited forever.
 *
 * The loss is a race with no seam to force it, so the test repeats it: on
 * 2026-10-19 (Linux, 16 CPUs, debug ZTS) it hung on 17 of 20 runs without the
 * fix and on 0 of 50 with it, and with an INFO log on stderr every one of 10
 * fixed runs logged reload.done with retired=1 to 4 at least once. Before
 * each reload every worker must have answered once (a per-thread id): a worker
 * still starting up when the reload is published is a different defect, and
 * waiting here keeps this test on the lost completion alone. The spread over
 * workers needs load-balanced SO_REUSEPORT, hence the SKIPIF.
 *
 * A watchdog turns a hang into a failure line instead of a timeout with the
 * scheduler's deadlock report repeated for megabytes. graceful_shutdown()
 * cancels it once, so it waits in a loop until start() has returned. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\delay;
use function Async\spawn;

const WORKERS = 4;
const RELOADS = 30;
const SPREAD_ATTEMPTS = 200;
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
    static $thread_id = null;
    $thread_id ??= bin2hex(random_bytes(4));
    $res->setStatusCode(200)->setBody($thread_id);
});

$stopped = false;

spawn(function () use (&$stopped) {
    $deadline = time() + WATCHDOG_S;

    while (!$stopped && time() < $deadline) {
        try {
            delay(200);
        } catch (\Async\AsyncCancellation) {
        }
    }

    /* exit() waits for the stuck start() like everything else; a signal does not. */
    if (!$stopped) {
        echo "watchdog: start() did not return in ", WATCHDOG_S, " s\n";
        shell_exec('kill -KILL ' . getmypid());
    }
});

spawn(function () use ($server, $port) {
    $ask = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/', $port));

    /* Distinct workers that answered, out of SPREAD_ATTEMPTS fresh connections. */
    $spread = static function () use ($ask): int {
        $seen = [];

        for ($i = 0; $i < SPREAD_ATTEMPTS && count($seen) < WORKERS; $i++) {
            $id = $ask();

            if ($id !== '') {
                $seen[$id] = true;
            } else {
                usleep(50000);
            }
        }

        return count($seen);
    };

    $reloaded = 0;

    for ($i = 0; $i < RELOADS; $i++) {
        $distinct = $spread();

        if ($distinct !== WORKERS) {
            echo "distinct=", $distinct, "/", WORKERS, " at reload ", $i, "\n";
            break;
        }

        if ($server->reload()) {
            $reloaded++;
        }
    }

    echo "reloaded=", $reloaded, "\n";

    \Async\graceful_shutdown();
});

$server->start();
$stopped = true;
echo "stopped=clean\n";
?>
--EXPECTF--
%Areloaded=30
%Astopped=clean%A
