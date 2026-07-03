--TEST--
WebSocket: permessage-deflate (RFC 7692) compressed echo round-trip
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
    ->setWsPermessageDeflate(true);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($msg = $ws->recv()) !== null) {
        $ws->send('echo: ' . $msg->data);
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

/* Build a masked client→server frame. RSV1 marks a compressed payload
 * (RFC 7692). client_no_context_takeover was negotiated, so each message
 * is compressed with a FRESH raw-deflate context. */
function ws_deflate_frame(string $payload, int $opcode = 0x1): string {
    $d = deflate_init(ZLIB_ENCODING_RAW);
    $comp = deflate_add($d, $payload, ZLIB_SYNC_FLUSH);
    $comp = substr($comp, 0, -4);              // drop 00 00 FF FF marker
    return ws_client_frame($comp, $opcode, true);
}

function ws_client_frame(string $payload, int $opcode, bool $rsv1): string {
    $b0  = 0x80 | ($rsv1 ? 0x40 : 0) | ($opcode & 0x0f);
    $len = strlen($payload);
    if ($len < 126) {
        $hdr = chr($b0) . chr(0x80 | $len);
    } elseif ($len < 65536) {
        $hdr = chr($b0) . chr(0x80 | 126) . pack('n', $len);
    } else {
        $hdr = chr($b0) . chr(0x80 | 127) . pack('J', $len);
    }
    $mask = 'abcd';
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return $hdr . $mask . $masked;
}

/* Read one server→client frame (server frames are never masked).
 * Returns ['opcode' => int, 'rsv1' => int, 'data' => string]. */
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
    } elseif ($len === 127) {
        $extra = fread($fp, 8);
        $len = 0;
        for ($i = 4; $i < 8; $i++) $len = ($len << 8) | ord($extra[$i]);
    }
    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === false || $c === '') break;
        $data .= $c;
    }
    return ['opcode' => $opcode, 'rsv1' => $rsv1, 'data' => $data];
}

/* Inflate a compressed server frame: append the synthetic tail and
 * inflate in a fresh context (server_no_context_takeover). */
function ws_inflate(string $payload): string {
    $i = inflate_init(ZLIB_ENCODING_RAW);
    return inflate_add($i, $payload . "\x00\x00\xff\xff", ZLIB_SYNC_FLUSH);
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);

    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
    . "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n\r\n");

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }
    $negotiated = stripos($hs, 'permessage-deflate') !== false ? 1 : 0;

    fwrite($fp, ws_deflate_frame('hello'));
    $r1 = read_server_frame($fp);

    fwrite($fp, ws_deflate_frame('second message'));
    $r2 = read_server_frame($fp);

    fclose($fp);
    usleep(50000);
    $server->stop();

    return [
        'negotiated' => $negotiated,
        'r1' => $r1,
        'r2' => $r2,
    ];
});

$server->start();
$result = await($client);

echo "negotiated=", $result['negotiated'], "\n";
echo "r1.opcode=", $result['r1']['opcode'], " r1.rsv1=", $result['r1']['rsv1'],
     " r1.data=", ws_inflate($result['r1']['data']), "\n";
echo "r2.opcode=", $result['r2']['opcode'], " r2.rsv1=", $result['r2']['rsv1'],
     " r2.data=", ws_inflate($result['r2']['data']), "\n";
echo "Done\n";
--EXPECT--
negotiated=1
r1.opcode=1 r1.rsv1=1 r1.data=echo: hello
r2.opcode=1 r2.rsv1=1 r2.data=echo: second message
Done
