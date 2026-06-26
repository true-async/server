--TEST--
Worker dispatch (#80, B1b/D7): request pointer -> handler coroutine on this thread -> response_wire
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_dispatch_from_wire_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Step B1b/D7: the worker side of the reactor/worker split. The hook builds a
 * persistent http_request_t (as the reactor will) and hands its pointer to
 * worker_dispatch_request (which wraps it in an HttpRequest and spawns the user
 * handler coroutine on THIS thread), waits for the handler to finish, and
 * returns the response rendered into a response_wire — the actor handoff ->
 * handler -> D3 round trip, minus the actual transport. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(new HttpServerConfig());
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(201)
        ->setHeader('content-type', 'text/plain')
        ->setHeader('x-echo', $req->getMethod() . ' ' . $req->getUri())
        ->setBody('hello-' . $req->getUri());
});

/* A second server with no handler registered: the dispatch must synthesise a
 * 404 so the sink still fires. */
$bare = new HttpServer(new HttpServerConfig());

$out = await(spawn(function () use ($server, $bare) {
    return [
        'get'  => _http_server_dispatch_from_wire_selftest(
            $server, 'GET', '/widgets/42', ['accept' => 'text/plain'], ''),
        'head' => _http_server_dispatch_from_wire_selftest(
            $server, 'HEAD', '/widgets/42', [], ''),
        'p404' => _http_server_dispatch_from_wire_selftest(
            $bare, 'GET', '/nope', [], ''),
    ];
}));

/* GET: handler ran on the spawned coroutine, request fields survived the wire. */
$g = $out['get'];
var_dump($g['status'] === 201);
var_dump($g['body'] === 'hello-/widgets/42');
var_dump($g['headers']['content-type'] === 'text/plain');
var_dump($g['headers']['x-echo'] === 'GET /widgets/42');

/* HEAD: headers rendered, body suppressed (RFC 9110 §9.3.2). */
$h = $out['head'];
var_dump($h['status'] === 201);
var_dump($h['body'] === '');
var_dump($h['headers']['x-echo'] === 'HEAD /widgets/42');

/* No handler -> synthesised 404. */
$p = $out['p404'];
var_dump($p['status'] === 404);
var_dump($p['body'] === 'Not Found');
var_dump($p['headers']['content-type'] === 'text/plain; charset=utf-8');

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
done
