--TEST--
WebSocket: WebSocketUpgrade::setSubprotocol() echoes Sec-WebSocket-Protocol in 101
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

$port = 19930 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req, WebSocketUpgrade $u) {
    $offers = $u->getOfferedSubprotocols();
    // Pick the second offered token to prove parsing works.
    if (count($offers) >= 2) {
        $u->setSubprotocol($offers[1]);
    }
    // Trigger commit so 101 actually goes on the wire.
    $ws->recv();
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
    . "Sec-WebSocket-Protocol: chat.v1, chat.v2, chat.v3\r\n"
    . "\r\n");
    $resp = '';
    while (!str_contains($resp, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $resp .= $c;
    }
    fclose($fp);
    usleep(50000);
    $server->stop();
    return $resp;
});

$server->start();
$resp = await($client);

$headers = [];
foreach (explode("\r\n", $resp) as $line) {
    if ($line === '') break;
    if (str_contains($line, ':')) {
        [$k, $v] = array_map('trim', explode(':', $line, 2));
        $headers[strtolower($k)] = $v;
    } else {
        $status = $line;
    }
}

echo "status:     ", $status, "\n";
echo "subprotocol: ", $headers['sec-websocket-protocol'] ?? '<none>', "\n";
echo "Done\n";
--EXPECT--
status:     HTTP/1.1 101 Switching Protocols
subprotocol: chat.v2
Done
