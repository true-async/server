--TEST--
WebSocket: permessage-deflate decompression bomb dies on the connection, worker survives
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

$port = 19890 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPermessageDeflate(true)
    ->setWsMaxMessageSize(65536);   // 64 KiB cap on the DECOMPRESSED size

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($msg = $ws->recv()) !== null) {
        $ws->send('echo: ' . $msg->data);
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function ws_deflate_frame(string $payload, int $opcode = 0x1): string {
    $d = deflate_init(ZLIB_ENCODING_RAW);
    $comp = deflate_add($d, $payload, ZLIB_SYNC_FLUSH);
    $comp = substr($comp, 0, -4);
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

function read_server_frame($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === false || $c === '') return ['opcode' => -1, 'data' => ''];
        $hdr .= $c;
    }
    $opcode = ord($hdr[0]) & 0x0f;
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
    return ['opcode' => $opcode, 'data' => $data];
}

function ws_inflate(string $payload): string {
    $i = inflate_init(ZLIB_ENCODING_RAW);
    return inflate_add($i, $payload . "\x00\x00\xff\xff", ZLIB_SYNC_FLUSH);
}

function ws_upgrade($port) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
    . "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }
    return $fp;
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);

    /* 1 MiB of zeros → ~1 KiB compressed (passes wslay's compressed cap),
     * inflates toward 1 MiB → smashes the 64 KiB decompressed cap. */
    $bomb = str_repeat("\0", 1024 * 1024);

    $fp = ws_upgrade($port);
    fwrite($fp, ws_deflate_frame($bomb));
    $r = read_server_frame($fp);
    /* The connection must die: either a 1009 Close frame (opcode 0x8) or a
     * straight EOF/RST (opcode -1). Anything else means the bomb slipped in. */
    $bomb_closed = ($r['opcode'] === 0x8 || $r['opcode'] === -1) ? 1 : 0;
    fclose($fp);

    /* Worker must still be alive: a fresh connection echoes normally. */
    usleep(20000);
    $fp2 = ws_upgrade($port);
    fwrite($fp2, ws_deflate_frame('ping'));
    $r2 = read_server_frame($fp2);
    $echo2 = ($r2['opcode'] === 0x1) ? ws_inflate($r2['data']) : '<none>';
    fclose($fp2);

    usleep(50000);
    $server->stop();

    return [
        'bomb_closed' => $bomb_closed,
        'echo2'       => $echo2,
    ];
});

$server->start();
$result = await($client);

echo "bomb_closed=", $result['bomb_closed'], "\n";
echo "echo2=", $result['echo2'], "\n";
echo "Done\n";
--EXPECT--
bomb_closed=1
echo2=echo: ping
Done
