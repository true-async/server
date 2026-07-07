--TEST--
HttpServer: setWorkers > 1 spawns Async\ThreadPool, parent start() awaits, workers serve traffic
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
/* Issue #11. setWorkers(N) > 1: HttpServer::start() builds an
 * Async\ThreadPool($N), submits N copies of a worker stub closure,
 * each transferring the server to its own thread, and awaits all
 * workers' completion. Each worker re-binds the same TCP listener;
 * the kernel load-balances accept() across them via SO_REUSEPORT.
 *
 * Verifies spin-up + serve, then exits via $server->stop(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$workers = 2;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers($workers);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('worker-tid=' . (function_exists('zend_thread_id') ? zend_thread_id() : '0'));
});

spawn(function () use ($port, $server) {
    /* Workers need a moment to thread up + bind. */
    usleep(400000);

    /* Hammer the port; SO_REUSEPORT round-robins to workers. */
    $hits = 0;
    for ($i = 0; $i < 16; $i++) {
        $out = (string) shell_exec(sprintf(
            'curl -s --max-time 2 http://127.0.0.1:%d/', $port));
        if (str_starts_with($out, 'worker-tid=')) {
            $hits++;
        }
    }
    echo "got_responses=", ($hits >= 1 ? 1 : 0), "\n";
    echo "done\n";
    $server->stop();
});

$server->start();
?>
--EXPECTF--
got_responses=1
done
%A
