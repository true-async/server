--TEST--
WebSocket: outbound message larger than ws_max_frame_size is auto-fragmented
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

$port = 19740 + getmypid() % 100;

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5)
    ->setWsMaxFrameSize(128);   // cap each frame at 128 bytes (setter minimum)

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->recv();                       // wait for the client's "go" (handshake flushed first)
    $ws->send(str_repeat('x', 300));   // 300 > 128 -> must split into fragments
    while ($ws->recv() !== null) {}
});
$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/** Read one frame; server frames are unmasked. Returns fin/opcode/len/data. */
function read_frame($fp): array {
    $h = '';
    while (strlen($h) < 2) { $c = fread($fp, 2 - strlen($h)); if ($c === '' || $c === false) return ['fin'=>true,'op'=>-1,'len'=>0,'data'=>'']; $h .= $c; }
    $fin = (ord($h[0]) & 0x80) !== 0;
    $op  = ord($h[0]) & 0x0f;
    $len = ord($h[1]) & 0x7f;
    if ($len === 126) { $e = fread($fp, 2); $len = (ord($e[0]) << 8) | ord($e[1]); }
    elseif ($len === 127) { $e = fread($fp, 8); $len = 0; for ($i = 4; $i < 8; $i++) $len = ($len << 8) | ord($e[$i]); }
    $data = '';
    while (strlen($data) < $len) { $c = fread($fp, $len - strlen($data)); if ($c === '' || $c === false) break; $data .= $c; }
    return ['fin'=>$fin, 'op'=>$op, 'len'=>$len, 'data'=>$data];
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

    // Send a masked "go" frame; the server replies with the big message,
    // so the 101 handshake has already been flushed on its own.
    $mask = "wxyz"; $p = "go"; $m = '';
    for ($i = 0; $i < 2; $i++) $m .= chr(ord($p[$i]) ^ ord($mask[$i & 3]));
    fwrite($fp, chr(0x81) . chr(0x82) . $mask . $m);

    $frames = [];
    $reassembled = '';
    $maxlen = 0;
    do {
        $f = read_frame($fp);
        if ($f['op'] === -1) break;
        $frames[] = $f;
        $reassembled .= $f['data'];
        $maxlen = max($maxlen, $f['len']);
    } while (!$f['fin'] && count($frames) < 20);

    fclose($fp);
    delay(50);
    $server->stop();
    return ['frames' => $frames, 'data' => $reassembled, 'maxlen' => $maxlen];
});

$server->start();
$r = await($client);

$f = $r['frames'];
echo "fragment_count=", count($f), "\n";
echo "first_opcode=0x", dechex($f[0]['op']), " first_fin=", $f[0]['fin'] ? '1' : '0', "\n";
echo "cont_opcode=0x", dechex($f[1]['op']), "\n";           // continuation
echo "last_fin=", $f[count($f)-1]['fin'] ? '1' : '0', "\n";
echo "max_fragment_len=", $r['maxlen'], " (cap=128)\n";
echo "reassembled_ok=", ($r['data'] === str_repeat('x', 300)) ? 'yes' : 'no', "\n";
echo "Done\n";
--EXPECT--
fragment_count=3
first_opcode=0x1 first_fin=0
cont_opcode=0x0
last_fin=1
max_fragment_len=128 (cap=128)
reassembled_ok=yes
Done
