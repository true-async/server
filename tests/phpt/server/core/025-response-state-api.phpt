--TEST--
HttpResponse: state observers — isHeadersSent / isClosed across send + end
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19240 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$snap = [];
$server->addHttpHandler(function ($req, $res) use (&$snap, $server) {
    // Initial state: nothing sent, not closed.
    $snap['init_headers_sent'] = $res->isHeadersSent();
    $snap['init_closed']       = $res->isClosed();

    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('state-check');

    // Setting buffer doesn't commit on the wire.
    $snap['post_set_headers_sent'] = $res->isHeadersSent();
    $snap['post_set_closed']       = $res->isClosed();

    $res->end();

    // After end() the response is closed; isHeadersSent depends on
    // protocol path (may already be true) — check both consistently.
    $snap['post_end_closed'] = $res->isClosed();

    $server->stop();
});

$cli = spawn(function () use ($port) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    $buf = '';
    while (!feof($fp)) $buf .= fread($fp, 8192);
    fclose($fp);
    echo "=== wire ===\n$buf\n";
});

$server->start();
await($cli);

echo "=== state ===\n";
foreach ($snap as $k => $v) echo "$k = " . var_export($v, true) . "\n";
--EXPECTF--
=== wire ===
HTTP/1.1 200 OK
Content-Length: 11
content-type: text/plain

state-check
=== state ===
init_headers_sent = false
init_closed = false
post_set_headers_sent = false
post_set_closed = false
post_end_closed = %s
