--TEST--
HttpServer: reload() guards — non-pool server throws; serving continues across reload under load (#93)
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

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

/* Guard: reload() outside pool mode is a hard error. */
require __DIR__ . '/../_free_port.inc';
$plain = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', tas_free_port()));
try {
    $plain->reload();
    echo "no-throw\n";
} catch (\Throwable $e) {
    echo "guard=", (strpos($e->getMessage(), 'worker pool') !== false ? 'ok' : $e->getMessage()), "\n";
}

/* Serving continuity: requests keep being answered across a rotation. */
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('pong');
});

spawn(function () use ($server, $port) {
    $curl = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/ 2>/dev/null', $port));

    $up = '';
    for ($i = 0; $i < 50 && $up !== 'pong'; $i++) {
        usleep(200000);
        $up = $curl();
    }

    echo "up=", $up, "\n";

    $ok = $server->reload();
    echo "reload=", var_export($ok, true), "\n";

    /* Hammer the port: it must come back and answer. */
    $hits = 0;
    for ($i = 0; $i < 60; $i++) {
        if ($curl() === 'pong') {
            $hits++;
            if ($hits >= 3) {
                break;
            }
        }

        usleep(200000);
    }

    echo "served_after=", ($hits >= 3 ? 'yes' : 'no'), "\n";
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
guard=ok
up=pong
%A
reload=true
served_after=yes%A
