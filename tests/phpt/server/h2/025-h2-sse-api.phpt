--TEST--
HttpResponse SSE API — HTTP/2 (h2c) event stream over DATA frames
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* Server-Sent Events over HTTP/2: the same first-class API drives the
 * h2 stream_ops (lazy HEADERS commit on first append_chunk, DATA frames
 * per event). Verifies the canonical SSE headers survive HPACK and the
 * WHATWG framing reassembles byte-for-byte on the client. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->sseStart();
    $res->sseEvent("hello");
    $res->sseEvent("multi\nline");
    $res->sseEvent("named", event: "ping", id: "7");
    $res->sseComment();
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -i -s --max-time 3 http://127.0.0.1:%d/events',
        $port
    );
    $raw = shell_exec($cmd) ?? '';

    $split = preg_split("/\r\n\r\n/", $raw, 2);
    $head  = $split[0] ?? '';
    $body  = $split[1] ?? '';

    $h = fn($needle) => stripos($head, $needle) !== false ? 'yes' : 'no';
    echo "ct=",  $h('content-type: text/event-stream'), "\n";
    echo "cc=",  $h('cache-control: no-cache, no-transform'), "\n";
    echo "xab=", $h('x-accel-buffering: no'), "\n";

    echo "---BODY---\n";
    echo $body;
    echo "---END---\n";

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
---BODY---
data: hello

data: multi
data: line

id: 7
event: ping
data: named

:

---END---
done
