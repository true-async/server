--TEST--
WebSocket: invalid UTF-8 fails fast with 1007 and the socket is torn down (no linger)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
// Regression guard for the Autobahn 6.4.x hang: on a protocol error wslay
// queues a CLOSE but wslay_event_recv returns 0 and surfaces no
// CONNECTION_CLOSE message, so the handler used to stay parked in recv()
// and the TCP socket lingered forever once the peer echoed the close.
// The server must instead close 1007 AND tear the connection down.
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketException;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    try {
        while ($ws->recv() !== null) {}
    } catch (WebSocketException $e) {
        // protocol error / abnormal close — end this handler
    }
});
$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/** Client→server frame (mask required, RFC 6455 §5.2). */
function fr(int $op, string $p, bool $fin): string {
    $mask = "wxyz"; $m = '';
    for ($i = 0, $n = strlen($p); $i < $n; $i++) $m .= chr(ord($p[$i]) ^ ord($mask[$i & 3]));
    return chr(($fin ? 0x80 : 0) | $op) . chr(0x80 | strlen($p)) . $mask . $m;
}

$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 3);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) { $c = fread($fp, 4096); if ($c === '' || $c === false) break; $hs .= $c; }

    // Autobahn 6.4.1 shape: valid fragment, then a continuation whose bytes
    // are an out-of-range codepoint (invalid UTF-8), both non-final.
    fwrite($fp, fr(0x1, "\xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5", false)); // "κόσμε"
    fwrite($fp, fr(0x0, "\xf4\x90\x80\x80", false));                            // invalid

    // Read the server's CLOSE frame.
    $h = fread($fp, 2);
    $op = ($h !== '' && $h !== false) ? (ord($h[0]) & 0x0f) : -1;
    $len = ($h !== '' && $h !== false) ? (ord($h[1]) & 0x7f) : 0;
    $d = $len ? fread($fp, $len) : '';
    $code = ($op === 0x8 && strlen($d) >= 2) ? ((ord($d[0]) << 8) | ord($d[1])) : 0;

    // Echo a CLOSE back (RFC close handshake) and confirm the server tears
    // the TCP down instead of lingering.
    fwrite($fp, fr(0x8, "\x03\xef", true));   // 1007
    $eof = fread($fp, 1);
    $meta = stream_get_meta_data($fp);
    $closed = ($eof === '' || $eof === false) && !$meta['timed_out'];

    fclose($fp);
    delay(50);
    $server->stop();
    return ['op' => $op, 'code' => $code, 'closed' => $closed];
});

$server->start();
$r = await($client);

echo "close_opcode=0x", dechex($r['op']), "\n";
echo "close_code=", $r['code'], "\n";
echo "server_closed_tcp=", $r['closed'] ? 'yes' : 'no', "\n";
echo "Done\n";
--EXPECT--
close_opcode=0x8
close_code=1007
server_closed_tcp=yes
Done
