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
 * This test only verifies the spin-up + serve path. Clean cross-thread
 * shutdown via $server->stop() on the parent is a follow-up — the test
 * exits hard after collecting responses. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19880 + getmypid() % 100;
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

spawn(function () use ($port) {
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
    /* Clean cross-thread broadcast is a follow-up (issue #11). Kill the
     * whole process — phpt only checks stdout up to this point. SIGKILL
     * skips PHP shutdown so worker threads can't deadlock the exit. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
got_responses=1
done
%A
