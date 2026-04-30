--TEST--
WebSocket: server-initiated close() emits a CLOSE frame with code + reason
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketCloseCode;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$port = 19880 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    // Receive one frame, close with PolicyViolation + reason.
    $msg = $ws->recv();
    $ws->close(WebSocketCloseCode::PolicyViolation, 'go away');
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function ws_client_text_frame(string $payload): string {
    $mask = "abcd";
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

/** Read one server frame; returns [opcode, payload]. */
function read_server_frame($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === false || $c === '') return [-1, ''];
        $hdr .= $c;
    }
    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;
    if ($len === 126) {
        $extra = fread($fp, 2);
        $len = (ord($extra[0]) << 8) | ord($extra[1]);
    } elseif ($len === 127) {
        $extra = fread($fp, 8);
        $len = 0;
        for ($i = 4; $i < 8; $i++) $len = ($len << 8) | ord($extra[$i]);
    }
    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === false || $c === '') break;
        $data .= $c;
    }
    return [$opcode, $data];
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
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    fwrite($fp, ws_client_text_frame('please close me'));

    [$opcode, $payload] = read_server_frame($fp);
    fclose($fp);
    usleep(50000);
    $server->stop();

    return [$opcode, $payload];
});

$server->start();
[$opcode, $payload] = await($client);

echo "opcode: 0x", dechex($opcode), "\n";   // expect 0x8 = CLOSE
$status = (ord($payload[0]) << 8) | ord($payload[1]);
$reason = substr($payload, 2);
echo "status: $status\n";                    // expect 1008 PolicyViolation
echo "reason: $reason\n";
echo "Done\n";
--EXPECT--
opcode: 0x8
status: 1008
reason: go away
Done
