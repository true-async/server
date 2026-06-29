--TEST--
WebSocket: recv() returns a WebSocketMessage from a single client text frame
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketMessage;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$port = 19860 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$captured = ['msg' => null, 'binary' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$captured) {
    $msg = $ws->recv();
    if ($msg !== null) {
        $captured['msg']    = $msg->data;
        $captured['binary'] = $msg->binary;
    }
    // Peer closes after sending one frame — recv() must return null
    // so the handler exits cleanly instead of suspending forever.
    $captured['second'] = $ws->recv();
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

/**
 * Build a client-to-server WebSocket text frame for `$payload`.
 * RFC 6455 §5.2 — FIN=1, opcode=1 (text), MASK=1, 7-bit-len for
 * payloads <= 125 bytes (which is fine for our smoke).
 */
function ws_client_text_frame(string $payload): string {
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);

    // Upgrade.
    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    // Drain the handshake response so the next read on the socket
    // (if we did one) wouldn't race with our send.
    stream_set_timeout($fp, 2);
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $hs .= $chunk;
    }

    // Send one text frame.
    fwrite($fp, ws_client_text_frame('hello server'));

    // Close — the server's recv loop should see graceful end after
    // delivering the one frame.
    fclose($fp);

    // Give the server time to process the FIN.
    usleep(50000);
    $server->stop();
});

$server->start();
await($client);

var_dump($captured['msg']);
var_dump($captured['binary']);
echo "second slot set: ", array_key_exists('second', $captured) ? 'yes' : 'no', "\n";
echo "second is null:  ", isset($captured) && array_key_exists('second', $captured)
                          && $captured['second'] === null ? 'yes' : 'no', "\n";
echo "Done\n";
--EXPECT--
string(12) "hello server"
bool(false)
second slot set: yes
second is null:  yes
Done
