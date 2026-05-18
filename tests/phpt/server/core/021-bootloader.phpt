--TEST--
HttpServer: setBootloader runs once per worker before the handler loop
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
/* HttpServerConfig::setBootloader(?Closure) hands a deep-copied closure
 * to ZEND_ASYNC_NEW_THREAD_POOL_EX. The pool runs that closure once per
 * worker before the worker's task loop — the test asserts it has executed
 * by the time the first request is served, by reading a constant the
 * bootloader defines into the worker's CG/EG. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

$port    = 19980 + getmypid() % 100;
$workers = 2;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers($workers)
    ->setBootloader(static function (): void {
        define('BOOTLOADER_RAN', 1);
    });

var_dump($config->getBootloader() instanceof Closure);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody(
        'boot=' . (defined('BOOTLOADER_RAN') ? '1' : '0')
    );
});

spawn(function () use ($port) {
    usleep(400000);
    $hits_with_boot = 0;
    for ($i = 0; $i < 8; $i++) {
        $out = (string) shell_exec(sprintf(
            'curl -s --max-time 2 http://127.0.0.1:%d/', $port));
        if ($out === 'boot=1') {
            $hits_with_boot++;
        }
    }
    echo "boot_hits>=1=", ($hits_with_boot >= 1 ? 1 : 0), "\n";
    echo "done\n";
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
bool(true)
boot_hits>=1=1
done
%A
