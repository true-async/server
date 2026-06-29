--TEST--
WebSocket: an inbound PING is answered with an automatic PONG (RFC 6455 §5.5.3)
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

$port = 19960 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0);   // disable server-initiated ping; isolate auto-PONG

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    // Control frames never surface to recv(); the handler just stays
    // alive (suspended) so the connection lives long enough to PONG.
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

/** Build a masked client control frame (FIN=1) for the given opcode. */
function ws_client_ctrl_frame(int $opcode, string $payload = ''): string {
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x80 | $opcode) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
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

    // Send a PING with a small payload; the server must echo it back as PONG.
    fwrite($fp, ws_client_ctrl_frame(0x9, 'pp'));
    [$opcode, $payload] = read_frame($fp);

    fclose($fp);
    usleep(20000);
    $server->stop();
    return [$opcode, $payload];
});

$server->start();
[$opcode, $payload] = await($client);

echo "pong opcode: 0x", dechex($opcode), "\n";  // expect 0xa PONG
echo "pong payload: ", $payload, "\n";          // RFC 6455 §5.5.3: echoes ping payload
echo "Done\n";
--EXPECT--
pong opcode: 0xa
pong payload: pp
Done
