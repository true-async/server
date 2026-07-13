--TEST--
WebSocket topics: a publish reaches permessage-deflate peers compressed (RSV1), and PMCE + plain peers side by side
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
/* Delivery to a topic is a fan-out over sessions, and each session negotiated its
 * OWN extensions: one peer may have permessage-deflate, the next may not. So a
 * publish cannot hand the same bytes to everyone — the compressed peer must get
 * an RSV1 frame it can inflate, the plain peer the raw text, from one publish().
 *
 * Nothing else in the suite covers topics over PMCE: 020/022 only echo. */
require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0)
    ->setWsPermessageDeflate(true);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');

    foreach ($ws as $msg) {
        $ws->publish('chat', 'relay:' . $msg->data);
    }
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

/* client_no_context_takeover was negotiated, so every message is deflated in a
 * FRESH raw context and the 00 00 FF FF tail is dropped. */
function ws_deflate_frame(string $payload): string {
    $d = deflate_init(ZLIB_ENCODING_RAW);
    $comp = deflate_add($d, $payload, ZLIB_SYNC_FLUSH);

    return ws_client_frame(substr($comp, 0, -4), 0x1, true);
}

function ws_client_frame(string $payload, int $opcode, bool $rsv1): string {
    $b0  = 0x80 | ($rsv1 ? 0x40 : 0) | ($opcode & 0x0f);
    $len = strlen($payload);

    if ($len < 126) {
        $hdr = chr($b0) . chr(0x80 | $len);
    } else {
        $hdr = chr($b0) . chr(0x80 | 126) . pack('n', $len);
    }

    $mask   = 'abcd';
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }

    return $hdr . $mask . $masked;
}

function read_server_frame($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === false || $c === '') return ['opcode' => -1, 'rsv1' => 0, 'data' => ''];
        $hdr .= $c;
    }

    $opcode = ord($hdr[0]) & 0x0f;
    $rsv1   = (ord($hdr[0]) >> 6) & 1;
    $len    = ord($hdr[1]) & 0x7f;

    if ($len === 126) {
        $extra = fread($fp, 2);
        $len = (ord($extra[0]) << 8) | ord($extra[1]);
    }

    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === false || $c === '') break;
        $data .= $c;
    }

    return ['opcode' => $opcode, 'rsv1' => $rsv1, 'data' => $data];
}

function ws_inflate(string $payload): string {
    $i = inflate_init(ZLIB_ENCODING_RAW);

    return inflate_add($i, $payload . "\x00\x00\xff\xff", ZLIB_SYNC_FLUSH);
}

/* $deflate = ask for permessage-deflate; otherwise a plain RFC 6455 peer. */
function ws_connect(int $port, bool $deflate) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
    if ($fp === false) return null;

    stream_set_timeout($fp, 3);

    $ext = $deflate
        ? "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n" : '';

    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
      . $ext . "\r\n");

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    return [$fp, stripos($hs, 'permessage-deflate') !== false];
}

$client = spawn(function () use ($port, $server) {
    delay(500);

    [$a, $aDeflate] = ws_connect($port, true);    // publisher, compressed
    [$b, $bDeflate] = ws_connect($port, true);    // compressed subscriber
    [$c, $cDeflate] = ws_connect($port, false);   // plain subscriber

    delay(300);   // let every subscribe land

    /* One publish, two peers that negotiated differently. */
    fwrite($a, ws_deflate_frame('hi'));

    $fromB = read_server_frame($b);
    $fromC = read_server_frame($c);

    fclose($a);
    fclose($b);
    fclose($c);

    delay(200);
    $server->stop();

    return compact('aDeflate', 'bDeflate', 'cDeflate', 'fromB', 'fromC');
});

$server->start();
$r = await($client);

echo 'publisher negotiated deflate: ', $r['aDeflate'] ? 'yes' : 'no', "\n";
echo 'peer B negotiated deflate: ',    $r['bDeflate'] ? 'yes' : 'no', "\n";
echo 'peer C stayed plain: ',          $r['cDeflate'] ? 'no' : 'yes', "\n";

echo 'B frame compressed (RSV1): ', $r['fromB']['rsv1'] === 1 ? 'yes' : 'no', "\n";
echo 'B inflates to: ', ws_inflate($r['fromB']['data']), "\n";

echo 'C frame compressed (RSV1): ', $r['fromC']['rsv1'] === 1 ? 'yes' : 'no', "\n";
echo 'C reads as: ', $r['fromC']['data'], "\n";
?>
--EXPECTF--
publisher negotiated deflate: yes
peer B negotiated deflate: yes
peer C stayed plain: yes
B frame compressed (RSV1): yes
B inflates to: relay:hi
C frame compressed (RSV1): no
C reads as: relay:hi%A
