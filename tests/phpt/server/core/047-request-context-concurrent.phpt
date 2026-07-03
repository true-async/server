--TEST--
HttpServer: per-request context survives concurrent in-flight requests (no cross-request clobbering)
--XFAIL--
Known bug true-async/server#74: when the server is destroyed with a per-request
handler coroutine still in-flight (here parked in Async\delay), the server scope
is disposed while a child per-request scope still holds that coroutine, tripping
the runtime assertion "Scope should be empty before disposal" (ext/async/scope.c).
Remove this XFAIL once graceful shutdown (drain/cancel-and-await before scope
disposal) lands.
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Two requests are deliberately kept in flight at the same time: the
 * handler writes a per-request marker into request_context(), then
 * suspends via Async\delay so the second request's handler runs and
 * writes ITS marker before the first resumes. If the per-request
 * scopes were shared (or reused without isolation) the marker read
 * back after the delay would be clobbered. With a private scope per
 * request, each handler reads back exactly the rid it wrote. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$done = 0;

$server->addHttpHandler(function ($req, $resp) use (&$done, $server) {
    $rid = ltrim($req->getUri(), '/');        // /x -> "x"

    $rc = Async\request_context();
    $rc->set('rid', $rid);

    // Yield long enough for the sibling request's handler to run and
    // write its own rid into ITS request context.
    delay(60);

    // Must still see our own rid — proves the contexts are isolated
    // even while both requests overlap in time.
    $after = $rc->get('rid');

    $resp->setStatusCode(200)
         ->setHeader('Content-Type', 'text/plain')
         ->setBody(sprintf("rid=%s after=%s match=%d\n",
             $rid, $after, ($after === $rid) ? 1 : 0));
    $resp->end();

    if (++$done >= 2) {
        $server->stop();
    }
});

/* Two separate TCP connections fired concurrently so both handlers are
 * in flight at once. */
function fetch(int $port, string $path): string {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        return "connect failed: $errstr";
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    $buf = '';
    while (!feof($fp)) {
        $buf .= fread($fp, 8192);
    }
    fclose($fp);
    return $buf;
}

$c1 = spawn(function () use ($port) { usleep(20000); return fetch($port, '/x'); });
$c2 = spawn(function () use ($port) { usleep(20000); return fetch($port, '/y'); });

$server->start();
$r1 = await($c1);
$r2 = await($c2);

$blob = $r1 . "\n" . $r2;
foreach (['x', 'y'] as $u) {
    echo "req_$u ok=", (int)(bool)preg_match("/rid=$u after=$u match=1/", $blob), "\n";
}
echo "Done\n";
--EXPECT--
req_x ok=1
req_y ok=1
Done
