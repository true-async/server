--TEST--
WebSocket over HTTP/2 (RFC 8441): permessage-deflate echo + bomb RSTs the stream, connection survives
--SKIPIF--
<?php
if (!extension_loaded('zlib')) die('skip zlib extension required');
try {
    (new TrueAsync\HttpServerConfig())->setCompressionEnabled(true);
} catch (\Throwable $e) {
    die('skip extension built without HTTP compression');
}
?>
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/../server/h2/_h2_client.inc';
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
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)
    ->setWsPermessageDeflate(true)
    ->setWsMaxMessageSize(65536);   // 64 KiB decompressed cap

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($m = $ws->recv()) !== null) {
        $ws->send('echo:' . $m->data);
    }
});
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

/** Masked, RSV1-flagged (compressed) client→server text frame. */
function ws_deflate_frame(string $payload): string {
    $d = deflate_init(ZLIB_ENCODING_RAW);
    $comp = substr(deflate_add($d, $payload, ZLIB_SYNC_FLUSH), 0, -4);
    $len = strlen($comp);
    $b0  = chr(0x80 | 0x40 | 0x01);   // FIN | RSV1 | text
    if ($len < 126)        $h = $b0 . chr(0x80 | $len);
    elseif ($len < 65536)  $h = $b0 . chr(0x80 | 126) . pack('n', $len);
    else                   $h = $b0 . chr(0x80 | 127) . pack('J', $len);
    $mask = random_bytes(4);
    $m = '';
    for ($i = 0; $i < $len; $i++) $m .= chr(ord($comp[$i]) ^ ord($mask[$i & 3]));
    return $h . $mask . $m;
}

function ws_inflate(string $p): string {
    $i = inflate_init(ZLIB_ENCODING_RAW);
    return inflate_add($i, $p . "\x00\x00\xff\xff", ZLIB_SYNC_FLUSH);
}

/** Decode an (unmasked) server WS frame: [opcode, rsv1, payload]. */
function parse_ws_server_frame(string $p): array {
    if (strlen($p) < 2) return [-1, 0, ''];
    $op   = ord($p[0]) & 0x0f;
    $rsv1 = (ord($p[0]) >> 6) & 1;
    $len  = ord($p[1]) & 0x7f;
    $off  = 2;
    if ($len === 126)      { $len = (ord($p[2]) << 8) | ord($p[3]); $off = 4; }
    elseif ($len === 127)  { $len = 0; for ($i = 2; $i < 10; $i++) $len = ($len << 8) | ord($p[$i]); $off = 10; }
    return [$op, $rsv1, substr($p, $off, $len)];
}

$client = spawn(function () use ($port) {
    usleep(60000);
    $cli = new H2TestClient('127.0.0.1', $port, 10);
    $ext = ['sec-websocket-extensions' => 'permessage-deflate'];

    /* --- Stream A: compressed echo round-trip. --- */
    $sidA = $cli->sendRequest('CONNECT', '/', 'localhost',
                              [':protocol' => 'websocket'] + $ext, null, false);
    $cli->sendRawFrame(H2_FRAME_DATA, 0, $sidA, ws_deflate_frame('hi-h2'));

    $echo = null;
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$t, $fl, $s, $pl] = $f;
        if ($t === H2_FRAME_SETTINGS && !($fl & H2_FLAG_ACK)) { $cli->sendSettingsAck(); continue; }
        if ($t === H2_FRAME_DATA && $s === $sidA && strlen($pl) >= 2) {
            [$op, $rsv1, $data] = parse_ws_server_frame($pl);
            $echo = ['op' => $op, 'rsv1' => $rsv1, 'data' => $rsv1 ? ws_inflate($data) : $data];
            break;
        }
    }

    /* --- Stream B: bomb. Wait for the 200 HEADERS (so the wslay session
     *     exists and inbound DATA goes through the live feed path) before
     *     sending the bomb, so the codec error RSTs the stream. --- */
    $sidB = $cli->sendRequest('CONNECT', '/', 'localhost',
                              [':protocol' => 'websocket'] + $ext, null, false);
    $accepted = false;
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$t, $fl, $s, $pl] = $f;
        if ($t === H2_FRAME_SETTINGS && !($fl & H2_FLAG_ACK)) { $cli->sendSettingsAck(); continue; }
        if ($t === H2_FRAME_HEADERS && $s === $sidB) { $accepted = true; break; }
    }

    $cli->sendRawFrame(H2_FRAME_DATA, 0, $sidB, ws_deflate_frame(str_repeat("\0", 1024 * 1024)));

    $bomb_closed = 0;
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$t, $fl, $s, $pl] = $f;
        if ($t === H2_FRAME_SETTINGS && !($fl & H2_FLAG_ACK)) { $cli->sendSettingsAck(); continue; }
        if ($s === $sidB && $t === H2_FRAME_RST_STREAM) { $bomb_closed = 1; break; }
        if ($s === $sidB && $t === H2_FRAME_DATA) {       // graceful CLOSE + END_STREAM also counts
            [$op, , ] = parse_ws_server_frame($pl);
            if ($op === 0x8 || ($fl & H2_FLAG_END_STREAM)) { $bomb_closed = 1; break; }
        }
    }

    /* --- Liveness: a PING ACK proves the worker + H2 connection survived. --- */
    $cli->sendPing('alive');
    $ping_ack = 0;
    $deadline = microtime(true) + 3;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$t, $fl, $s, $pl] = $f;
        if ($t === H2_FRAME_PING && ($fl & H2_FLAG_ACK)) { $ping_ack = 1; break; }
    }

    $cli->close();
    return ['echo' => $echo, 'accepted' => $accepted,
            'bomb_closed' => $bomb_closed, 'ping_ack' => $ping_ack];
});

$stopper = spawn(function () use ($server, $client) {
    await($client);
    usleep(40000);
    if ($server->isRunning()) { $server->stop(); }
});

spawn(function () use ($server) {       // safety net
    usleep(6000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$r = await($client);
await($stopper);

echo "echo opcode: 0x", ($r['echo'] ? dechex($r['echo']['op']) : '?'), "\n";
echo "echo rsv1: ", ($r['echo']['rsv1'] ?? '?'), "\n";
echo "echo payload: ", ($r['echo']['data'] ?? '<none>'), "\n";
echo "bomb stream closed: ", $r['bomb_closed'], "\n";
echo "connection alive (ping ack): ", $r['ping_ack'], "\n";
echo "Done\n";
--EXPECT--
echo opcode: 0x1
echo rsv1: 1
echo payload: echo:hi-h2
bomb stream closed: 1
connection alive (ping ack): 1
Done
