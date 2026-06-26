--TEST--
Worker inbox (#80, B2/D7): N request pointers through the #81 mailbox -> dispatch -> N responses
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_worker_inbox_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Step B2/D7: the worker consumer side. A worker_inbox is a #81 mailbox whose
 * drain feeds each posted http_request_t to worker_dispatch_request (B1b). The
 * hook posts N synthetic request pointers (as a reactor would), then waits for
 * the drain to dispatch them and the handlers to render all N responses. Proves
 * the mailbox -> dispatch -> response path carries N independent requests. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$server = new HttpServer(new HttpServerConfig());
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok-' . $req->getUri());
});

$count = 50;

$r = await(spawn(fn () => _http_server_worker_inbox_selftest($server, $count)));

var_dump($r['expected'] === $count);
var_dump($r['received'] === $count);  /* every posted wire was drained + dispatched */
var_dump($r['ok'] === $count);        /* every handler rendered a correct 200 response */

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
done
