--TEST--
HttpServer: reload() rotates the worker pool — fresh workers pick up changed code (#93)
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
/* The bootloader defines APP_VERSION from a data file at worker boot; the
 * handler serves it. Bump the file, call reload() on the pool parent — the
 * old cohort drains and exits, replacements re-run the bootloader and serve
 * the new value on the same port. Boot marks prove a full rotation (2 -> 4). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port  = tas_free_port();
$vfile = sys_get_temp_dir() . '/tas_reload_ver_' . getmypid();
$bfile = sys_get_temp_dir() . '/tas_reload_boot_' . getmypid();
file_put_contents($vfile, 'v1');
@unlink($bfile);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2)
    ->setBootloader(static function () use ($vfile, $bfile): void {
        file_put_contents($bfile, 'x', FILE_APPEND | LOCK_EX);
        define('APP_VERSION', trim((string)@file_get_contents($vfile)));
    });

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody(APP_VERSION);
});

spawn(function () use ($server, $port, $vfile, $bfile) {
    $curl = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/ 2>/dev/null', $port));

    /* Wait for the first cohort to serve. */
    $v1 = '';
    for ($i = 0; $i < 50 && $v1 !== 'v1'; $i++) {
        usleep(200000);
        $v1 = $curl();
    }

    echo "before=", $v1, "\n";

    file_put_contents($vfile, 'v2');
    $ok = $server->reload();   // suspends until the old cohort has drained
    echo "reload=", var_export($ok, true), "\n";

    /* Replacements may still be binding — retry until the new code answers. */
    $v2 = '';
    for ($i = 0; $i < 50 && $v2 !== 'v2'; $i++) {
        usleep(200000);
        $v2 = $curl();
    }

    echo "after=", $v2, "\n";
    echo "boots=", strlen((string)@file_get_contents($bfile)), "\n";

    @unlink($vfile);
    @unlink($bfile);
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
before=v1
%A
reload=true
after=v2
boots=4%A
