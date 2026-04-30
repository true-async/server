--TEST--
WebSocket: client-fragmented message is reassembled into one recv()
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

$port = 19900 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$captured = ['msg' => null, 'binary' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$captured) {
    $msg = $ws->recv();
    if ($msg !== null) {
        $captured['msg']    = $msg->data;
        $captured['binary'] = $msg->binary;
    }
    $ws->recv();
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/** Build a single fragment frame. fin/opcode are caller's responsibility. */
function ws_fragment(int $fin_bit, int $opcode, string $payload): string {
    $b0 = ($fin_bit ? 0x80 : 0x00) | ($opcode & 0x0f);
    $mask = "abcd";
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr($b0) . chr(0x80 | strlen($payload)) . $mask . $masked;
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

    // Three fragments: text "abc" + continuation "def" + final continuation "ghi".
    // RFC 6455 §5.4: first frame has the data opcode (0x1), subsequent
    // continuations have opcode 0x0; only the last has FIN=1.
    fwrite($fp, ws_fragment(0, 0x1, "abc"));
    fwrite($fp, ws_fragment(0, 0x0, "def"));
    fwrite($fp, ws_fragment(1, 0x0, "ghi"));

    usleep(80000);   // give server time to reassemble + recv
    fclose($fp);
    usleep(50000);
    $server->stop();
});

$server->start();
await($client);

var_dump($captured['msg']);
var_dump($captured['binary']);
echo "Done\n";
--EXPECT--
string(9) "abcdefghi"
bool(false)
Done
