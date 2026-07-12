--TEST--
Stats gate (#5, A3): setStatsEnabled round-trips; slab only allocated when enabled
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
if (!function_exists('_http_server_stats_slab_snapshot')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Stage A3: statistics are opt-in. The config flag round-trips, defaults off,
 * and a pool started with it OFF must allocate no slab (snapshot stays empty). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

/* Config round-trip + default. */
$c = new HttpServerConfig();
echo 'default_off=', ($c->isStatsEnabled() === false ? 1 : 0), "\n";
$c->setStatsEnabled(true);
echo 'set_on=', ($c->isStatsEnabled() === true ? 1 : 0), "\n";
$c->setStatsEnabled(false);
echo 'set_off=', ($c->isStatsEnabled() === false ? 1 : 0), "\n";

/* Pool with stats OFF: no slab, snapshot empty. */
$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2);           /* stats NOT enabled */

$server = new HttpServer($config);
$server->addHttpHandler(fn ($req, $res) => $res->setStatusCode(200)->setBody('OK'));

spawn(function () use ($server) {
    usleep(400000);
    $slots = _http_server_stats_slab_snapshot();
    echo 'no_slab_when_disabled=', (count($slots) === 0 ? 1 : 0), "\n";
    echo "done\n";
    $server->stop();
});

$server->start();
?>
--EXPECTF--
default_off=1
set_on=1
set_off=1
no_slab_when_disabled=1
done
%A
