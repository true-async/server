--TEST--
WebSocket: non-GET upgrade request is rejected with 405 + Allow: GET
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

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
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
    // RFC 6455 §4.1 step 1: the handshake MUST be a GET. POST is rejected.
    fwrite($fp,
      "POST / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
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
echo "allow: ", (stripos($resp, "Allow: GET") !== false) ? 'yes' : 'no', "\n";
echo "Done\n";
--EXPECT--
status: HTTP/1.1 405 Method Not Allowed
allow: yes
Done
