--TEST--
HttpResponse: helper methods — json/html/redirect set Content-Type and body
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

// One server, three requests against three different paths — each path
// triggers a different helper. Server stops after the third response.

$port = 19230 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$count = 0;
$server->addHttpHandler(function ($req, $res) use (&$count, $server) {
    $path = $req->getUri();
    if ($path === '/json') {
        $res->json(['k' => 'v', 'n' => 42]);
    } elseif ($path === '/html') {
        $res->html('<h1>hi</h1>');
    } elseif ($path === '/redirect') {
        $res->redirect('/elsewhere', 301);
    } else {
        $res->setStatusCode(404)->setBody('nf');
    }
    $res->end();
    if (++$count >= 3) $server->stop();
});

$cli = spawn(function () use ($port) {
    usleep(20000);
    foreach (['/json', '/html', '/redirect'] as $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        $buf = '';
        while (!feof($fp)) $buf .= fread($fp, 8192);
        fclose($fp);
        echo "=== $path ===\n$buf\n";
    }
});

$server->start();
await($cli);
--EXPECTF--
=== /json ===
HTTP/1.1 200 OK
Content-Length: 16
content-type: application/json%A

{"k":"v","n":42}
=== /html ===
HTTP/1.1 200 OK
Content-Length: 11
content-type: text/html%A

<h1>hi</h1>
=== /redirect ===
HTTP/1.1 301 Moved Permanently
Content-Length: 0
location: /elsewhere

