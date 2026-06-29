--TEST--
WebSocket: binary frames round-trip via sendBinary() / recv()
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

$port = 19890 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $msg = $ws->recv();
    if ($msg !== null) {
        // Echo back as binary regardless of input opcode, with a
        // distinguishing prefix.
        $ws->sendBinary("BIN:" . $msg->data);
    }
    $ws->recv();   // wait for peer close
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/** Build a client→server frame. opcode 0x1 = text, 0x2 = binary. */
function ws_frame(string $payload, int $opcode = 0x1): string {
    $mask = "wxyz";
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x80 | $opcode) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

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
    // Send a binary frame containing some non-UTF-8 bytes.
    fwrite($fp, ws_frame("\x00\x01\x02\xFF\xFE", 0x2));
    [$opcode, $data] = read_frame($fp);
    fclose($fp);
    usleep(50000);
    $server->stop();
    return [$opcode, $data];
});

$server->start();
[$opcode, $data] = await($client);

echo "opcode: 0x", dechex($opcode), "\n";   // expect 0x2 binary
echo "prefix: ", substr($data, 0, 4), "\n"; // expect BIN:
echo "tail (hex): ", bin2hex(substr($data, 4)), "\n";  // expect 00 01 02 ff fe
echo "Done\n";
--EXPECT--
opcode: 0x2
prefix: BIN:
tail (hex): 000102fffe
Done
