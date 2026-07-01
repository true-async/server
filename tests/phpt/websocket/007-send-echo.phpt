--TEST--
WebSocket: send() echoes a text frame back to the client end-to-end
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

$port = 19870 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($msg = $ws->recv()) !== null) {
        $ws->send('echo: ' . $msg->data);
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

/** Build a client→server text frame (mask required by RFC 6455 §5.2). */
function ws_client_text_frame(string $payload): string {
    $mask = "abcd";
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

/**
 * Read one server→client frame. Server frames are NEVER masked
 * (RFC 6455 §5.1) so we just decode opcode + length + payload.
 * Returns ['opcode' => int, 'data' => string].
 */
function read_server_frame($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === false || $c === '') return ['opcode' => -1, 'data' => ''];
        $hdr .= $c;
    }
    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;
    if ($len === 126) {
        $extra = fread($fp, 2);
        $len = (ord($extra[0]) << 8) | ord($extra[1]);
    } elseif ($len === 127) {
        $extra = fread($fp, 8);
        // 32-bit cap is plenty for the test
        $len = 0;
        for ($i = 4; $i < 8; $i++) $len = ($len << 8) | ord($extra[$i]);
    }
    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === false || $c === '') break;
        $data .= $c;
    }
    return ['opcode' => $opcode, 'data' => $data];
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);

    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    // drain handshake response
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    fwrite($fp, ws_client_text_frame('hi'));
    $r1 = read_server_frame($fp);

    fwrite($fp, ws_client_text_frame('there'));
    $r2 = read_server_frame($fp);

    fclose($fp);
    usleep(50000);
    $server->stop();

    return ['r1' => $r1, 'r2' => $r2];
});

$server->start();
$result = await($client);

echo "r1.opcode=", $result['r1']['opcode'], " data=", $result['r1']['data'], "\n";
echo "r2.opcode=", $result['r2']['opcode'], " data=", $result['r2']['data'], "\n";
echo "Done\n";
--EXPECT--
r1.opcode=1 data=echo: hi
r2.opcode=1 data=echo: there
Done
