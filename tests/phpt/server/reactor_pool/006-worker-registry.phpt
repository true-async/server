--TEST--
Worker registry (#80, B3): round-robin dispatch across N worker inboxes
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_worker_registry_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Step B3 (producer side, part 1): the worker_registry is the table a transport
 * reactor reads to pick which worker to hand a request to. The hook publishes N
 * inboxes, posts `count` requests through worker_registry_pick (round-robin),
 * and reports how many each inbox handled. With count a multiple of N the spread
 * must be exactly even. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(new HttpServerConfig());
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok-' . $req->getUri());
});

$workers = 4;
$count   = 40;

$r = await(spawn(fn () => _http_server_worker_registry_selftest($server, $workers, $count)));

var_dump($r['expected'] === $count);
var_dump($r['received'] === $count);   /* every request dispatched + rendered */
var_dump($r['ok'] === $count);         /* every response correct */
var_dump(count($r['distribution']) === $workers);
var_dump(array_sum($r['distribution']) === $count);
var_dump(min($r['distribution']) === 10 && max($r['distribution']) === 10); /* exact RR spread */

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
