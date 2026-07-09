--TEST--
WebSocket: inbound FIFO byte cap — flooding a stalled consumer closes 1013, memory stays bounded
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\WebSocketClosedException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsMaxMessageSize(1024);   /* FIFO cap = 8× = 8 KiB */

$server = new HttpServer($config);

$outcome = ['code' => null, 'overflowed' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$outcome) {
    /* Stalled consumer: let the client flood past the 8 KiB cap. */
    delay(300);

    try {
        while ($ws->recv() !== null) {
            /* drain whatever fit under the cap */
        }
        $outcome['overflowed'] = false;
    } catch (WebSocketClosedException $e) {
        $outcome['overflowed'] = true;
        $outcome['code'] = $e->closeCode;
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

/* Client text frame, 16-bit length form for payloads > 125 bytes. */
function ws_client_text_frame(string $payload): string {
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    $len = strlen($payload);
    $hdr = chr(0x81);
    if ($len <= 125) {
        $hdr .= chr(0x80 | $len);
    } else {
        $hdr .= chr(0x80 | 126) . pack('n', $len);
    }
    return $hdr . $mask . $masked;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);

    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    stream_set_timeout($fp, 2);
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $hs .= $chunk;
    }

    /* 24 × 700 B ≈ 16.4 KiB — twice the 8 KiB cap. */
    $frame = ws_client_text_frame(str_repeat('x', 700));
    for ($i = 0; $i < 24; $i++) {
        fwrite($fp, $frame);
    }

    /* Server must answer with CLOSE 1013 and drop the connection. */
    $close_code = null;
    $buf = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $buf .= $chunk;
        if (strlen($buf) >= 4 && (ord($buf[0]) & 0x0f) === 0x8) {
            $close_code = unpack('n', substr($buf, 2, 2))[1];
            break;
        }
    }
    fclose($fp);

    echo "client saw close: ", $close_code === 1013 ? '1013' : var_export($close_code, true), "\n";

    delay(200);
    $server->stop();
});

$server->start();
await($client);

echo "handler overflowed: ", $outcome['overflowed'] ? 'yes' : 'no', "\n";
echo "handler code: ", $outcome['code'], "\n";
echo "Done\n";
--EXPECT--
client saw close: 1013
handler overflowed: yes
handler code: 1013
Done
