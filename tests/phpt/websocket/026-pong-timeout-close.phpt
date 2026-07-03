--TEST--
WebSocket: peer that never PONGs is closed 1001 after ws_pong_timeout_ms
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

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(100)    // ping fast
    ->setWsPongTimeoutMs(150);    // demand a PONG within 150ms

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    // Block until the server tears the connection down on the missed pong.
    while ($ws->recv() !== null) {}
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/** Read one server frame; return [opcode, payload]. Server frames are
 * never masked (RFC 6455 §5.1); control payloads are always < 126 bytes. */
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
    stream_set_timeout($fp, 3);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }

    // Deliberately never PONG. Collect frames until the server CLOSEs.
    $got_ping   = false;
    $close_code = null;
    for ($i = 0; $i < 10; $i++) {
        [$opcode, $data] = read_frame($fp);
        if ($opcode === 0x9) { $got_ping = true; continue; }   // server PING
        if ($opcode === 0x8) {                                  // CLOSE
            if (strlen($data) >= 2) {
                $close_code = (ord($data[0]) << 8) | ord($data[1]);
            }
            break;
        }
        if ($opcode === -1) break;   // socket closed without a CLOSE frame
    }

    fclose($fp);
    delay(50);
    $server->stop();
    return ['ping' => $got_ping, 'code' => $close_code];
});

$server->start();
$r = await($client);

echo "got_ping: ", $r['ping'] ? 'yes' : 'no', "\n";
echo "close_code: ", $r['code'] ?? 'none', "\n";
echo "Done\n";
--EXPECT--
got_ping: yes
close_code: 1001
Done
