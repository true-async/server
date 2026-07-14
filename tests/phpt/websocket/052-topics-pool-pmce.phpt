--TEST--
WebSocket topics: a publish crossing workers is compressed by the RECEIVING worker, per session
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
/* Compression is negotiated PER SESSION, and a session never leaves its thread —
 * so a publish cannot travel between workers already compressed. It crosses as
 * raw bytes and the RECEIVING worker deflates it for each of its own peers, with
 * that peer's window bits and its own no-context-takeover state.
 *
 * 046 pins permessage-deflate, but on one worker, where the publisher's own
 * thread does the deflating. Nothing pinned the cross-worker path: a payload
 * accidentally compressed once at the source and refcount-shared across the
 * fan-out would decode as garbage on the far side, or not at all. Eight peers
 * over four workers here, so at least one of them is being served by a worker
 * that did not run the publish(). */
require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const PEERS = 7;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)
    ->setWsPermessageDeflate(true);

$server = new HttpServer($config);

/* Self-contained: a worker thread gets a fresh PHP context and does not inherit
 * this script's constants. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');

    foreach ($ws as $msg) {
        $ws->publish('chat', 'relay:' . $msg->data);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

/* client_no_context_takeover: every message deflates in a FRESH raw context and
 * the 00 00 FF FF tail is dropped. */
function pmce_frame(string $payload): string {
    $d    = deflate_init(ZLIB_ENCODING_RAW);
    $comp = substr(deflate_add($d, $payload, ZLIB_SYNC_FLUSH), 0, -4);

    $b0  = 0x80 | 0x40 | 0x1;   // FIN | RSV1 | text
    $len = strlen($comp);

    $hdr = $len < 126
        ? chr($b0) . chr(0x80 | $len)
        : chr($b0) . chr(0x80 | 126) . pack('n', $len);

    $mask   = 'abcd';
    $masked = '';
    for ($i = 0; $i < $len; $i++) {
        $masked .= chr(ord($comp[$i]) ^ ord($mask[$i & 3]));
    }

    return $hdr . $mask . $masked;
}

function pmce_read($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === false || $c === '') return ['rsv1' => 0, 'data' => ''];
        $hdr .= $c;
    }

    $rsv1 = (ord($hdr[0]) >> 6) & 1;
    $len  = ord($hdr[1]) & 0x7f;

    if ($len === 126) {
        $len = unpack('n', fread($fp, 2))[1];
    }

    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === false || $c === '') break;
        $data .= $c;
    }

    return ['rsv1' => $rsv1, 'data' => $data];
}

function pmce_inflate(string $payload): string {
    $i = inflate_init(ZLIB_ENCODING_RAW);

    return (string) inflate_add($i, $payload . "\x00\x00\xff\xff", ZLIB_SYNC_FLUSH);
}

function pmce_open(int $port) {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 3);
    if ($fp === false) return null;

    stream_set_timeout($fp, 5);
    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
      . "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n\r\n");

    /* One byte at a time: a block read would swallow frames that arrived in the
     * same segment as the 101. */
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 1);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }

    return stripos($hs, 'permessage-deflate') !== false ? $fp : null;
}

spawn(function () use ($port, $server) {
    delay(4000);   // four workers have to bind

    $conns = [];
    for ($i = 0; $i <= PEERS; $i++) {
        $fp = pmce_open($port);

        if ($fp === null) { echo "handshake or PMCE failed\n"; $server->stop(); return; }

        $conns[] = $fp;
    }

    delay(1000);   // let every subscribe land in its worker's tree

    /* A payload worth compressing, so RSV1 is not a coin toss. */
    $payload = str_repeat('topics-over-workers ', 20);

    fwrite($conns[0], pmce_frame($payload));

    $compressed = 0;
    $decoded    = 0;

    for ($i = 1; $i <= PEERS; $i++) {
        $frame = pmce_read($conns[$i]);

        $compressed += $frame['rsv1'] === 1 ? 1 : 0;
        $decoded    += pmce_inflate($frame['data']) === 'relay:' . $payload ? 1 : 0;
    }

    echo 'peers served: ', PEERS, "\n";
    echo 'frames marked compressed (RSV1): ', $compressed, "\n";
    echo 'frames that inflate to the payload: ', $decoded, "\n";

    foreach ($conns as $fp) { fclose($fp); }

    $server->stop();
});

$server->start();
?>
--EXPECTF--
peers served: 7
frames marked compressed (RSV1): 7
frames that inflate to the payload: 7%A
