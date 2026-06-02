--TEST--
HttpServerConfig: setRequestScope(false) is honored across workers > 1
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip POSIX-only test (worker fan-out + SIGKILL teardown)');
if (!extension_loaded('posix')) die('skip posix extension not available');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* The per-request scope knob must survive the cross-thread shared-config
 * snapshot. With workers > 1 each worker rebuilds its config from the frozen
 * snapshot (freeze + populate_from_shared); if request_scope is not carried
 * through, setRequestScope(false) is silently ignored on the worker and it
 * keeps the default-on behavior. We assert the worker honors scope-OFF by
 * checking that Async\request_context() resolves to null there (scope ON
 * would yield a non-null per-request context). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

$port = 19985 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setRequestScope(false)
    ->setWorkers(2);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $ctx = \Async\request_context();
    $res->setStatusCode(200)->setBody($ctx === null ? "ctx=null\n" : "ctx=set\n");
});

spawn(function () use ($port) {
    usleep(400000);  // let the pool thread up and the worker adopt its config
    $url = sprintf('http://127.0.0.1:%d/', $port);
    $body = trim((string) shell_exec(sprintf('curl -s --max-time 2 %s', escapeshellarg($url))));
    echo "worker_scope_off=", ($body === 'ctx=null' ? 'yes' : "no($body)"), "\n";
    echo "done\n";

    /* Issue #11: clean cross-thread shutdown is a follow-up. SIGKILL skips
     * PHP shutdown so worker threads cannot deadlock on exit. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
worker_scope_off=yes
done
%A
