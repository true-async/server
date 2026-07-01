--TEST--
WebSocket: WebSocketUpgrade::reject() sends 4xx instead of 101
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketUpgrade;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$port = 19920 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req, WebSocketUpgrade $u) {
    if (!$req->hasHeader('authorization')) {
        $u->reject(401, 'auth required');
    }
    // Handler returns; dispose path emits the 4xx because reject_status != 0.
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $resp = '';
    while (($c = fread($fp, 4096)) !== '' && $c !== false) {
        $resp .= $c;
        if (str_contains($resp, "\r\n\r\n")) break;
    }
    fclose($fp);
    usleep(50000);
    $server->stop();
    return $resp;
});

$server->start();
$resp = await($client);

$lines = explode("\r\n", $resp);
echo "status: ", $lines[0], "\n";    // expect HTTP/1.1 401 Unauthorized
echo "Done\n";
--EXPECT--
status: HTTP/1.1 401 Unauthorized
Done
