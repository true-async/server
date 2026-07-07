--TEST--
StaticHandler: workers > 1 — every worker serves the static mount (issue #13)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* Issue #13 / TODO §1: addStaticHandler must survive the worker-pool
 * fan-out so every worker thread serves /static/* in C.  Before the fix
 * that mounted only the parent server, workers cloned via transfer_obj
 * had static_handler_count == 0 and every static request fell through
 * to the (here non-existent) PHP handler -> synthetic 404. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;

$root = sys_get_temp_dir() . '/static-w-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
file_put_contents("$root/hello.txt", "static-from-worker");

register_shutdown_function(function() use ($root) {
    @unlink("$root/hello.txt");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$workers = 2;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers($workers);

$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/static/', $root))->disableIndex()
);

spawn(function () use ($port) {
    /* Workers need a moment to thread up + bind. */
    usleep(400000);

    $hits = 0;
    for ($i = 0; $i < 16; $i++) {
        $out = (string) shell_exec(sprintf(
            'curl -s --max-time 2 http://127.0.0.1:%d/static/hello.txt', $port));
        if ($out === 'static-from-worker') {
            $hits++;
        }
    }
    /* SO_REUSEPORT load-balances accepts; with both workers correctly
     * mounting the static handler, all 16 requests should land on a
     * worker that serves the file body byte-exact. */
    echo "hits=", ($hits === 16 ? 'all' : (string) $hits), "\n";
    echo "done\n";
    /* $server->stop() currently aborts here: a static-handler libuv handle
     * survives worker teardown and trips the ext/async debug assert
     * "The event loop must be stopped" (scheduler.c fiber_entry). SIGKILL
     * until that teardown leak is fixed. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
hits=all
done
%A
