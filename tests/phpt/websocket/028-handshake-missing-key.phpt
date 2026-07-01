--TEST--
WebSocket: missing Sec-WebSocket-Key is rejected with 400
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19980 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->recv();
});
$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    // Valid version, but no Sec-WebSocket-Key (RFC 6455 §4.1 step 5/6).
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Version: 13\r\n\r\n");
    $resp = '';
    while (($c = fread($fp, 4096)) !== '' && $c !== false) {
        $resp .= $c;
        if (str_contains($resp, "\r\n\r\n")) break;
    }
    fclose($fp);
    delay(50);
    $server->stop();
    return $resp;
});

$server->start();
$resp = await($client);

$lines = explode("\r\n", $resp);
echo "status: ", $lines[0], "\n";
echo "Done\n";
--EXPECT--
status: HTTP/1.1 400 Bad Request
Done
