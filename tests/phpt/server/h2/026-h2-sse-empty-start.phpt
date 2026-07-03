--TEST--
HttpResponse SSE API — sseStart() with no event commits an empty 200 (HTTP/2)
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
/* sseStart() is the only way to reach streaming mode with no chunk ever
 * appended. The first append_chunk is what lazily commits the HEADERS
 * frame, so a start-then-close with no event used to leave H2/H3 without
 * any HEADERS at all — the client saw a reset stream instead of the empty
 * 200 H1 already delivered. h2_stream_mark_ended now commits the empty
 * streaming response (ring init -> HEADERS -> EOF) when nothing was sent,
 * matching H1. */

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
    $res->sseStart();   // no event, no comment
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
    echo "status_200=", (stripos($head, 'HTTP/2 200') !== false || stripos($head, ' 200') !== false) ? 'yes' : 'no', "\n";
    echo "ct=",  $h('content-type: text/event-stream'), "\n";
    echo "body_empty=", $body === '' ? 'yes' : 'no', "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
status_200=yes
ct=yes
body_empty=yes
done
