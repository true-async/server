--TEST--
HttpServerConfig::enableReloadOnSignal — SIGHUP rotates the pool, serving continues (#93)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip no SIGHUP on Windows');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port  = tas_free_port();
$bfile = sys_get_temp_dir() . '/tas_sighup_boot_' . getmypid();
@unlink($bfile);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2)
    ->setBootloader(static function () use ($bfile): void {
        file_put_contents($bfile, 'x', FILE_APPEND | LOCK_EX);
    })
    ->enableReloadOnSignal();

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('pong');
});

/* Old workers log "shutting down"/"exited" to stderr from their own threads as
 * they drain within the grace window — unordered vs the new workers booting and
 * vs these echoes, so those lines may land after boots=4. EXPECTF trails each
 * marker with %A to absorb them; do not drop the %A. */

spawn(function () use ($port, $bfile) {
    $curl = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/ 2>/dev/null', $port));

    $up = '';
    for ($i = 0; $i < 50 && $up !== 'pong'; $i++) {
        usleep(200000);
        $up = $curl();
    }

    echo "up=", $up, "\n";

    posix_kill(getmypid(), SIGHUP);

    /* Rotation done when both replacements have re-run the bootloader. */
    $boots = 0;
    for ($i = 0; $i < 100 && $boots < 4; $i++) {
        usleep(200000);
        $boots = strlen((string) @file_get_contents($bfile));
    }

    echo "boots=", $boots, "\n";

    $again = '';
    for ($i = 0; $i < 50 && $again !== 'pong'; $i++) {
        usleep(200000);
        $again = $curl();
    }

    echo "served_after=", $again, "\n";

    @unlink($bfile);
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
up=pong%A
boots=4%A
served_after=pong%A
