--TEST--
HttpServer: request_context() is per-stream under HTTP/2 multiplex (and shared across the stream subtree)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* Each multiplexed HTTP/2 stream spawns its own handler coroutine in
 * its own per-request scope (http_request_handler_coroutine_new). This
 * test proves, under 3 concurrent streams on ONE TCP connection, that:
 *   - request_context() resolves to a context private to each stream
 *     (the rid written by stream /a never leaks into /b or /c);
 *   - a child coroutine in a NESTED scope still sees the same
 *     per-stream request context (subtree inheritance);
 *   - in the handler current_context() IS the request context, while
 *     the nested child's own context differs.
 *
 * curl --parallel --parallel-immediate over prior-knowledge h2 opens
 * one TCP and multiplexes /a /b /c as separate streams. The handler
 * derives a deterministic rid from the URI so responses correlate
 * regardless of interleave. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$served = 0;

$server->addHttpHandler(function ($req, $resp) use (&$served, $server) {
    $rid = ltrim($req->getUri(), '/');           // /a -> "a"

    $cur = Async\current_context();
    $rc  = Async\request_context();
    $rc->set('rid', $rid);

    // Child in a nested scope (parent = this per-request scope). Hold
    // the scope in a local so it is not disposed before the child runs.
    $scope = \Async\Scope::inherit();
    $child = $scope->spawn(function () {
        $own = Async\current_context();
        $reqc = Async\request_context();
        return [
            'rid'         => $reqc->get('rid'),
            'own_differs' => ($own !== $reqc) ? 1 : 0,
        ];
    });
    $cd = await($child);

    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'text/plain')
         ->setBody(sprintf(
             "uri=/%s rc_rid=%s child_rid=%s handler_is_req=%d child_own_differs=%d\n",
             $rid, $rc->get('rid'), $cd['rid'],
             ($cur === $rc) ? 1 : 0, $cd['own_differs']
         ));

    if (++$served >= 3) {
        $server->stop();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s --parallel --parallel-immediate '
        . '--max-time 3 '
        . 'http://127.0.0.1:%d/a http://127.0.0.1:%d/b http://127.0.0.1:%d/c',
        $port, $port, $port
    );
    $out = [];
    exec($cmd, $out, $rc);
    $blob = implode("\n", $out);

    echo "rc=$rc\n";
    foreach (['a', 'b', 'c'] as $u) {
        // Each stream: its request_context rid == its own URI (no cross-stream
        // leak), the nested child saw the same rid, identity holds in handler,
        // and the child's own context differs from the request context.
        $ok = (bool) preg_match(
            "/uri=\\/$u rc_rid=$u child_rid=$u handler_is_req=1 child_own_differs=1/",
            $blob
        );
        echo "stream_$u ok=", (int)$ok, "\n";
    }
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
rc=0
stream_a ok=1
stream_b ok=1
stream_c ok=1
Done
