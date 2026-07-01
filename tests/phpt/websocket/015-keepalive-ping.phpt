--TEST--
WebSocket: server-initiated PING fires on the configured interval
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

$port = 19940 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(100);   // 100ms — fast for tests

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    // Stay alive long enough to emit at least one PING frame.
    $ws->recv();
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/** Read one server frame; return [opcode, payload]. */
function read_frame($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === '' || $c === false) return [-1, ''];
        $hdr .= $c;
    }
    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;
    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === '' || $c === false) break;
        $data .= $c;
    }
    return [$opcode, $data];
}

$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }
    // Wait for the first PING — at 100ms interval one should arrive
    // within ~150ms; allow more headroom for slow CI.
    [$opcode, $_] = read_frame($fp);

    fclose($fp);
    delay(50);
    $server->stop();
    return $opcode;
});

$server->start();
$opcode = await($client);

echo "first server frame opcode: 0x", dechex($opcode), "\n"; // expect 0x9 PING
echo "Done\n";
--EXPECT--
first server frame opcode: 0x9
Done
