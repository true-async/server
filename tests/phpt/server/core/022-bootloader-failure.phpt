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
 * server serves nothing: the parent must name the reason and start() must
 * report failure instead of the success of a run that never happened.
 *
 * Reporting the exception itself belongs to the pool and is pinned there
 * (php-async, tests/thread_pool/080-bootloader_exception_reported.phpt). The
 * fatal it prints lands in this test's output too, between the lines below,
 * on any build new enough to carry it. */

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
%A[true-async-server] worker did not start: RuntimeException: boot failed!%Abool(false)%A
