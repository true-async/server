--TEST--
HttpResponse: status / reason API — setStatusCode/getStatusCode/setReasonPhrase/getReasonPhrase
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19210 + getmypid() % 1000;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$probe = [];
$server->addHttpHandler(function ($req, $res) use (&$probe, $server) {
    // Default before any setStatusCode call
    $probe['default_status'] = $res->getStatusCode();
    $probe['default_reason'] = $res->getReasonPhrase();

    // Standard code → canonical reason
    $res->setStatusCode(404);
    $probe['s404_code'] = $res->getStatusCode();
    $probe['s404_reason'] = $res->getReasonPhrase();

    // Custom reason override
    $res->setReasonPhrase('Teapot Rebellion');
    $probe['custom_reason'] = $res->getReasonPhrase();

    // Numeric custom code
    $res->setStatusCode(418);
    $probe['s418_code'] = $res->getStatusCode();
    // After setStatusCode, reason gets re-derived (or kept — observe)
    $probe['s418_reason'] = $res->getReasonPhrase();

    // Final wire response: 201 Created with custom reason
    $res->setStatusCode(201)->setReasonPhrase('Birthed')
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('born');
    $res->end();
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

echo "=== probe ===\n";
foreach ($probe as $k => $v) echo "$k = " . var_export($v, true) . "\n";
--EXPECTF--
=== wire ===
HTTP/1.1 201 Birthed
Content-Length: 4
content-type: text/plain

born
=== probe ===
default_status = %d
default_reason = %s
s404_code = 404
s404_reason = %s
custom_reason = 'Teapot Rebellion'
s418_code = 418
s418_reason = %s
