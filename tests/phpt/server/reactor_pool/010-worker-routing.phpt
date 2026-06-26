--TEST--
Worker routing (#80, D5): reactor-paired strided ownership + idle spread + global fallback
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_worker_registry_route_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Step D5 (dispatch policy): worker_registry_least_busy is the primitive behind
 * reactor-paired sticky dispatch. A reactor owns a STRIDED subset of slots
 * {i : i % n_reactors == reactor_id}; among its idle (depth-0) owned workers the
 * pick rotates so connections spread; reactor_id < 0 scans ALL slots (the global
 * spill / fallback); unpublished slots are skipped; an owned set with nothing
 * published returns NULL (which is what makes the dispatch path fall back to
 * global). All inboxes here are idle, so the math is fully deterministic. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(new HttpServerConfig());
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$route = fn (...$a) => await(spawn(fn () =>
    _http_server_worker_registry_route_selftest($server, ...$a)));

/* 1. Reactor 1 of 4, 8 workers all published: owns slots {1,5} and rotates between
 *    them; never picks a non-owned slot. */
$r = $route(8, 8, 4, 1, 200);
$d = $r['distribution'];
$owned  = $d[1] + $d[5];
$others = array_sum($d) - $owned;
var_dump($r['none'] === 0);
var_dump($others === 0);
var_dump($d[1] > 0 && $d[5] > 0);

/* 2. Global (reactor_id -1): exact spread across all 8 idle slots. */
$r = $route(8, 8, 4, -1, 800);
$d = $r['distribution'];
var_dump($r['none'] === 0);
var_dump(min($d) > 0);
var_dump(array_sum($d) === 800);

/* 3. Reactor 2 owns {2,6}, but only slots 0,1 are published -> owned set empty ->
 *    route returns NULL every time (the global-fallback trigger in dispatch). */
$r = $route(8, 2, 4, 2, 100);
var_dump($r['none'] === 100);
var_dump(array_sum($r['distribution']) === 0);

/* 4. Global fallback lands only on published slots, skipping the unpublished. */
$r = $route(8, 2, 4, -1, 200);
$d = $r['distribution'];
var_dump($r['none'] === 0);
var_dump($d[0] > 0 && $d[1] > 0);
var_dump(array_sum(array_slice($d, 2)) === 0);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
