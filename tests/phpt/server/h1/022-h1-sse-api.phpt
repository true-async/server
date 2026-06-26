--TEST--
HttpResponse SSE API — HTTP/1.1 chunked framing, headers, streaming lock
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
/* End-to-end Server-Sent Events over HTTP/1.1 chunked. Exercises the
 * first-class API (sseStart / sseComment / sseEvent / sseRetry) and
 * verifies:
 *   - the three canonical SSE headers are emitted by sseStart(),
 *   - Transfer-Encoding: chunked is selected automatically,
 *   - WHATWG §9.2 framing (single-line fields, multiline data split,
 *     comment, retry directive, conventional field order),
 *   - the response is locked into streaming mode after start
 *     (setHeader + a second sseStart throw). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19880 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->sseStart();
    $res->sseComment();                          // heartbeat -> ":\n\n"
    $res->sseEvent("hello");                      // data: hello
    $res->sseEvent("line1\nline2");               // two data: lines
    $res->sseEvent("tick", event: "ping", id: "42");
    $res->sseRetry(5000);                         // retry: 5000

    $lock = [];
    try { $res->setHeader('X-Late', '1'); $lock[] = 'setHeader:no-throw'; }
    catch (Throwable $e) { $lock[] = 'setHeader:throw'; }
    try { $res->sseStart(); $lock[] = 'start:no-throw'; }
    catch (Throwable $e) { $lock[] = 'start:throw'; }
    $res->sseEvent(implode(',', $lock));

    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http1.1 -i -s -N --max-time 3 http://127.0.0.1:%d/events',
        $port
    );
    $raw = shell_exec($cmd) ?? '';

    $split = preg_split("/\r\n\r\n/", $raw, 2);
    $head  = $split[0] ?? '';
    $body  = $split[1] ?? '';

    $h = fn($needle) => stripos($head, $needle) !== false ? 'yes' : 'no';
    echo "ct=",  $h('Content-Type: text/event-stream'), "\n";
    echo "cc=",  $h('Cache-Control: no-cache, no-transform'), "\n";
    echo "xab=", $h('X-Accel-Buffering: no'), "\n";
    echo "te=",  $h('Transfer-Encoding: chunked'), "\n";

    echo "---BODY---\n";
    echo $body;
    echo "---END---\n";

    $t = $server->getTelemetry();
    echo "sends=", $t['stream_send_calls_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
ct=yes
cc=yes
xab=yes
te=yes
---BODY---
:

data: hello

data: line1
data: line2

id: 42
event: ping
data: tick

retry: 5000

data: setHeader:throw,start:throw

---END---
sends=6
done
