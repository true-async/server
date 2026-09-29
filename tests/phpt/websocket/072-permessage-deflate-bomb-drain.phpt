--TEST--
WebSocket: a decompression bomb from a peer still sending is drained — the peer finishes its flood and reads CLOSE 1009
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
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPermessageDeflate(true)
    ->setWsMaxMessageSize(65536);   /* cap on the DECOMPRESSED size */

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while ($ws->recv() !== null) {
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function ws_client_frame(string $payload, bool $rsv1): string {
    $b0  = 0x81 | ($rsv1 ? 0x40 : 0);
    $len = strlen($payload);
    if ($len < 126) {
        $hdr = chr($b0) . chr(0x80 | $len);
    } else {
        $hdr = chr($b0) . chr(0x80 | 126) . pack('n', $len);
    }
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return $hdr . $mask . $masked;
}

/* No usleep() or microtime() below: run-tests retries a FILE section that calls
 * either and reports only the retry, which hides a close lost one run in two. */
$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
      . "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n");

    stream_set_timeout($fp, 2);
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $hs .= $chunk;
    }

    /* 1 MiB of zeros compresses to about 1 KiB and inflates past the 64 KiB
     * cap. The 8000 uncompressed frames after it (≈ 5.4 MiB) keep the peer
     * sending well past the close, past what the two socket buffers hold: a
     * close with those bytes unread resets the connection, which fails the
     * rest of the flood here and, on Windows, discards the 1009 as well. */
    $d = deflate_init(ZLIB_ENCODING_RAW);
    $bomb = substr(deflate_add($d, str_repeat("\0", 1024 * 1024), ZLIB_SYNC_FLUSH), 0, -4);
    fwrite($fp, ws_client_frame($bomb, true));

    $frame = ws_client_frame(str_repeat('x', 700), false);
    $wrote = 0;
    for ($i = 0; $i < 8000; $i++) {
        $w = @fwrite($fp, $frame);
        if ($w === false || $w === 0) { break; }
        $wrote++;
    }

    /* Attempts, not a clock: only an empty read waits, and the timeout below
     * caps it, so 60 attempts is about 13 s against a peer that never answers. */
    $close_code = null;
    stream_set_timeout($fp, 0, 200000);
    $attempts_left = 60;

    while ($attempts_left-- > 0) {
        $f = ws_read_frame($fp);

        if ($f === null) {
            if (feof($fp)) {
                break;
            }

            delay(20);
            continue;
        }

        if ($f['opcode'] === 0x8) {
            $close_code = strlen($f['data']) >= 2 ? unpack('n', substr($f['data'], 0, 2))[1] : 0;
            break;
        }
    }

    fclose($fp);

    echo "client wrote the flood: ", $wrote === 8000 ? 'all' : "$wrote of 8000", "\n";
    echo "client saw close: ", var_export($close_code, true), "\n";

    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
client wrote the flood: all
client saw close: 1009
Done
