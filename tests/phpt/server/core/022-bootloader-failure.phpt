--TEST--
HttpServer: a bootloader that throws is reported and start() answers false
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* A bootloader failure kills every worker before it reaches accept(), so the
 * server serves nothing. Two things must follow from that: the exception is
 * reported through the worker's own error stream (the pool otherwise carries it
 * only in the task rejection, which this server never awaits), and start()
 * reports failure instead of the success of a run that never happened. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

require_once __DIR__ . '/../_free_port.inc';

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', tas_free_port())
    ->setWorkers(2)
    ->setBootloader(static function (): void {
        throw new \RuntimeException('boot failed!');
    });

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('never served');
});

var_dump($server->start());
?>
--EXPECTF--
%AUncaught RuntimeException: boot failed!%Abool(false)%A
