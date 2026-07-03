--TEST--
WebSocket: getRemoteAddress() returns the peer host:port
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

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    // Report the peer address back as a text frame, then exit.
    $ws->send($ws->getRemoteAddress());
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
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    // Read the handshake ONE BYTE at a time so we don't swallow the
    // text frame the handler sends immediately after the 101.
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 1);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }
    [$opcode, $payload] = read_frame($fp);
    fclose($fp);
    usleep(20000);
    $server->stop();
    return [$opcode, $payload];
});

$server->start();
[$opcode, $payload] = await($client);

echo "opcode: 0x", dechex($opcode), "\n";                       // 0x1 text
echo "matches 127.0.0.1:<port>: ",
     (preg_match('/^127\.0\.0\.1:\d{1,5}$/', $payload) ? 'yes' : "no ($payload)"), "\n";
echo "Done\n";
--EXPECT--
opcode: 0x1
matches 127.0.0.1:<port>: yes
Done
