--TEST--
HttpResponse SSE API — mixing send() and SSE throws (symmetric sse_mode guard)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
/* send() and the sse* helpers drive the same streaming pipeline but in
 * incompatible framings. Once one side commits the stream the other must
 * refuse rather than silently corrupt it:
 *   - send() first  -> sseEvent()/sseComment()/sseRetry() throw (the
 *     stream is plain, not text/event-stream),
 *   - sseStart()/sseEvent() first -> send() throws (the stream is SSE).
 * Both raise HttpServerRuntimeException; the handler can catch it and keep
 * streaming through the channel it already committed to. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19700 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$mark = function (callable $fn): string {
    try { $fn(); return 'no-throw'; }
    catch (Throwable $e) {
        $cls   = $e::class;
        $short = substr($cls, strrpos($cls, '\\') + 1);
        $m     = $e->getMessage();
        $kind  = str_contains($m, 'already streaming via send()') ? 'sse-after-send'
               : (str_contains($m, 'in SSE mode')                 ? 'send-in-sse'
               : 'other');
        return "$short:$kind";
    }
};

$server->addHttpHandler(function ($req, $res) use ($mark) {
    if ($req->getPath() === '/sse-then-send') {
        $res->sseStart();
        $k = $mark(fn () => $res->send("x"));   // SSE committed -> send() throws
        $res->sseEvent($k);                      // report back over SSE
        $res->end();
        return;
    }

    // /send-then-sse
    $res->send("a=");                            // plain stream committed
    $k = $mark(fn () => $res->sseEvent("x"));    // -> sseEvent() throws
    $res->send($k);                              // report back over the plain stream
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $base = sprintf('curl --http1.1 -s --max-time 3 http://127.0.0.1:%d', $port);
    echo "A:", shell_exec($base . '/send-then-sse') ?? '', "\n";
    echo "B:", shell_exec($base . '/sse-then-send') ?? '', "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
A:a=HttpServerRuntimeException:sse-after-send
B:data: HttpServerRuntimeException:send-in-sse


done
