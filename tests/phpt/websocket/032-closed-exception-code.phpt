--TEST--
WebSocket: recv() throws WebSocketClosedException carrying closeCode/closeReason on an error close
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketClosedException;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$captured = ['thrown' => false, 'code' => null, 'reason' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$captured) {
    try {
        $ws->recv();                       // peer will send CLOSE 1008
        $captured['thrown'] = 'no-null';   // graceful null (should not happen)
    } catch (WebSocketClosedException $e) {
        $captured['thrown'] = true;
        $captured['code']   = $e->closeCode;    // readonly prop must be readable
        $captured['reason'] = $e->closeReason;
    }
});
$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) { $c = fread($fp, 4096); if ($c === '' || $c === false) break; $hs .= $c; }

    // CLOSE frame, code 1008 (Policy Violation) + reason "nope", masked.
    $payload = "\x03\xf0" . "nope";       // 0x03f0 = 1008
    $mask = "wxyz"; $m = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) $m .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    fwrite($fp, chr(0x88) . chr(0x80 | strlen($payload)) . $mask . $m);

    usleep(80000);
    fclose($fp);
    delay(50);
    $server->stop();
});

$server->start();
await($client);

echo "thrown=", var_export($captured['thrown'], true), "\n";
echo "closeCode=", var_export($captured['code'], true), "\n";
echo "closeReason=", var_export($captured['reason'], true), "\n";
echo "Done\n";
--EXPECT--
thrown=true
closeCode=1008
closeReason='nope'
Done
