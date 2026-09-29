--TEST--
WebSocket: a CLOSE queued while the handler is parked in its own send still reaches the peer
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
require_once __DIR__ . '/_ws_client.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)
    ->setWsMaxMessageSize(1024);   /* FIFO cap = 8× = 8 KiB */

$server = new HttpServer($config);

$outcome = ['code' => null, 'sent' => null];

/* 16 MiB is past what the two socket buffers hold, so the send parks in the
 * transport write until the client reads. The flood arrives meanwhile and
 * overflows the inbound cap, which queues CLOSE 1013 behind the message the
 * handler is still writing. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$outcome) {
    try {
        $ws->send(str_repeat('x', 16 * 1024 * 1024));
        $outcome['sent'] = true;

        while ($ws->recv() !== null) {
        }
    } catch (WebSocketClosedException $e) {
        $outcome['code'] = $e->closeCode;
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function ws_client_text_frame(string $payload): string {
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x81) . chr(0x80 | 126) . pack('n', strlen($payload)) . $mask . $masked;
}

/* No usleep() or microtime() below: run-tests retries a FILE section that calls
 * either and reports only the retry. */
$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    stream_set_timeout($fp, 5);
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $hs .= $chunk;
    }
    ws_pushback($fp, substr($hs, strpos($hs, "\r\n\r\n") + 4));

    /* 24 × 704 B ≈ 16.5 KiB — twice the 8 KiB cap. */
    $frame = ws_client_text_frame(str_repeat('y', 700));
    for ($i = 0; $i < 24; $i++) {
        fwrite($fp, $frame);
    }

    /* The 16 MiB message first, in fragments, then whatever follows it: the
     * CLOSE, or the end of the connection without one. */
    $close_code = null;
    $big = 0;
    for ($frames = 0; $frames < 100000; $frames++) {
        $f = ws_read_frame($fp);

        if ($f === null) {
            break;
        }

        if ($f['opcode'] === 0x1 || $f['opcode'] === 0x0) {
            $big += strlen($f['data']);
        } elseif ($f['opcode'] === 0x8) {
            $close_code = strlen($f['data']) >= 2 ? unpack('n', substr($f['data'], 0, 2))[1] : 0;
            break;
        }
    }

    fclose($fp);

    echo "client read the message: ", $big === 16 * 1024 * 1024 ? 'whole' : $big, "\n";
    echo "client saw close: ", var_export($close_code, true), "\n";

    delay(200);
    $server->stop();
});

$server->start();
await($client);

echo "handler finished its send: ", $outcome['sent'] ? 'yes' : 'no', "\n";
echo "handler code: ", $outcome['code'], "\n";
echo "Done\n";
--EXPECT--
client read the message: whole
client saw close: 1013
handler finished its send: yes
handler code: 1013
Done
