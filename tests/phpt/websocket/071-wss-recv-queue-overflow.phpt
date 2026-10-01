--TEST--
WebSocket: wss inbound FIFO byte cap — the flooding peer reads CLOSE 1013 over TLS
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../server/tls/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'php_ssl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/../server/tls/_tls_skipif.inc';
require_once __DIR__ . '/../server/_free_port.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use TrueAsync\WebSocketClosedException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp = __DIR__ . '/tmp-wss-071';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

/* The client runs out of process: its TLS handshake is synchronous and would
 * block the server loop. No sleep or clock call anywhere in this section:
 * run-tests retries a FILE section that names one and reports only the retry,
 * which hides a close lost one run in two. */
$client_php = $tmp . '/wss_flood_client.php';
file_put_contents($client_php, <<<'CLIENT'
<?php
$port = (int)$argv[1];
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$fp = @stream_socket_client("ssl://127.0.0.1:$port", $e, $s, 4, STREAM_CLIENT_CONNECT, $ctx);
if (!$fp) { echo "CONNECT_FAIL $s\n"; exit(1); }
stream_set_timeout($fp, 4);
fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
$buf = '';
while (!str_contains($buf, "\r\n\r\n")) {
    $c = fread($fp, 4096);
    if ($c === '' || $c === false) { echo "NO_HANDSHAKE\n"; exit(1); }
    $buf .= $c;
}
$buf = substr($buf, strpos($buf, "\r\n\r\n") + 4);

/* 8000 × 706 B ≈ 5.4 MiB: past the 8 KiB cap at once, and past what the two
 * socket buffers hold, so the server closes while most of it is unsent. A
 * close with those bytes unread resets the connection. Linux still hands the
 * reader what arrived before the reset, Windows discards it, so the CLOSE
 * code alone proves the drain only on Windows; the write that fails midway
 * proves the reset everywhere. */
$payload = str_repeat('x', 700);
$mask = random_bytes(4);
$masked = '';
for ($i = 0; $i < 700; $i++) { $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3])); }
$frame = chr(0x81) . chr(0x80 | 126) . pack('n', 700) . $mask . $masked;
$wrote = 0;
for ($i = 0; $i < 8000; $i++) {
    $w = @fwrite($fp, $frame);
    if ($w === false || $w === 0) { break; }
    $wrote++;
}

/* Frames accepted before the cap may precede the close, so the stream is
 * walked frame by frame until a CLOSE, EOF or the read timeout. */
$code = null;
while (true) {
    while (strlen($buf) >= 2) {
        $len = ord($buf[1]) & 0x7f;
        $hdr = 2;
        if ($len === 126) {
            if (strlen($buf) < 4) { break; }
            $len = unpack('n', substr($buf, 2, 2))[1];
            $hdr = 4;
        } elseif ($len === 127) {
            if (strlen($buf) < 10) { break; }
            $len = unpack('J', substr($buf, 2, 8))[1];
            $hdr = 10;
        }
        if (strlen($buf) < $hdr + $len) { break; }
        $opcode = ord($buf[0]) & 0x0f;
        $data = substr($buf, $hdr, $len);
        $buf = substr($buf, $hdr + $len);
        if ($opcode === 0x8) {
            $code = strlen($data) >= 2 ? unpack('n', substr($data, 0, 2))[1] : 0;
            break 2;
        }
    }
    $c = @fread($fp, 8192);
    if ($c === '' || $c === false) { break; }
    $buf .= $c;
}
fclose($fp);
echo "client wrote the flood: ", $wrote === 8000 ? 'all' : "$wrote of 8000", "\n";
echo "client saw close: ", $code === null ? 'NULL' : $code, "\n";
CLIENT
);

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0)
    ->setWsMaxMessageSize(1024);   /* FIFO cap = 8× = 8 KiB */

$server = new HttpServer($config);

$outcome = ['code' => null, 'overflowed' => null];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$outcome) {
    /* The 101 goes out on the first WebSocket I/O, so the send commits the
     * upgrade and the delay after it is a consumer that is really stalled:
     * a delay before any I/O would hold the 101 back, and the flood would
     * meet a handler already draining in recv(). */
    $ws->send('go');
    delay(300);

    try {
        while ($ws->recv() !== null) {
            /* drain whatever fit under the cap */
        }
        $outcome['overflowed'] = false;
    } catch (WebSocketClosedException $e) {
        $outcome['overflowed'] = true;
        $outcome['code'] = $e->closeCode;
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

$client = spawn(function () use ($port, $client_php, $server) {
    delay(80);
    $out = shell_exec(escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($client_php) . " $port 2>&1");
    delay(200);
    $server->stop();

    return trim((string) $out);
});

$server->start();
$out = await($client);

@unlink($cert); @unlink($key); @unlink($client_php); @rmdir($tmp);

echo $out, "\n";
echo "handler overflowed: ", $outcome['overflowed'] ? 'yes' : 'no', "\n";
echo "handler code: ", $outcome['code'], "\n";
echo "Done\n";
--EXPECT--
client wrote the flood: all
client saw close: 1013
handler overflowed: yes
handler code: 1013
Done
