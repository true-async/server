--TEST--
WebSocket topics over TLS: a publish reaches a second wss:// peer
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
/* Topics deliver through ws_session_try_send, which writes through whatever
 * transport the session was built on — so a publish has to come out the far side
 * of the TLS layer just like a send() does. Nothing else pins that: 018 only
 * echoes over wss, and every topic test so far has been plaintext. */
require_once __DIR__ . '/../server/tls/_tls_skipif.inc';
require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-wss-topics';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

/* Out of process: a synchronous TLS handshake in-process would block the server
 * loop. One process, two wss:// sockets — A publishes, B must hear it. */
$client_php = $tmp . '/wss_topics_client.php';
file_put_contents($client_php, <<<'CLIENT'
<?php
$host = $argv[1]; $port = (int)$argv[2];

function wss_open(string $host, int $port) {
    $ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
    $fp = @stream_socket_client("ssl://$host:$port", $e, $s, 4, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) { return null; }
    stream_set_timeout($fp, 4);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: $host\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    /* Byte at a time: reading in blocks would swallow frames that arrived in the
     * same TLS record as the 101. */
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 1);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }
    return str_contains($hs, ' 101 ') ? $fp : null;
}

function wss_send($fp, string $msg): void {
    $mask = random_bytes(4); $n = strlen($msg); $m = '';
    for ($i = 0; $i < $n; $i++) { $m .= chr(ord($msg[$i]) ^ ord($mask[$i & 3])); }
    fwrite($fp, chr(0x81) . chr(0x80 | $n) . $mask . $m);
}

function wss_read($fp): string {
    $h = fread($fp, 2);
    if ($h === false || strlen($h) < 2) { return ''; }
    $len = ord($h[1]) & 0x7f;
    $p = '';
    while (strlen($p) < $len) {
        $c = fread($fp, $len - strlen($p));
        if ($c === '' || $c === false) break;
        $p .= $c;
    }
    return $p;
}

$a = wss_open($host, $port);
$b = wss_open($host, $port);

if ($a === null || $b === null) { echo "HANDSHAKE_FAIL\n"; exit(1); }

usleep(300000);            // let both subscribes land
wss_send($a, 'over-tls');

$onB = wss_read($b);       // must be the relayed publish

/* A published with excludeSelf, so it must hear nothing. */
stream_set_blocking($a, false);
$onA = wss_read($a);

fclose($a); fclose($b);

echo "B=$onB A=", ($onA === '' ? '<silent>' : $onA), "\n";
CLIENT
);

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setWorkers(1)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');

    foreach ($ws as $m) {
        $ws->publish('chat', 'relay:' . $m->data);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $client_php) {
    usleep(80000);
    $cmd = escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($client_php)
         . " 127.0.0.1 $port 2>&1";

    return shell_exec($cmd);
});

$stopper = spawn(function () use ($server, $client) {
    await($client);
    usleep(30000);
    if ($server->isRunning()) { $server->stop(); }
});

spawn(function () use ($server) {   // safety net
    usleep(8000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$out = trim((string) await($client));
await($stopper);

@unlink($cert); @unlink($key); @unlink($client_php); @rmdir($tmp);

echo 'peer B got the publish over TLS: ',
     str_contains($out, 'B=relay:over-tls') ? 'yes' : "no ($out)", "\n";
echo 'publisher excluded: ',
     str_contains($out, 'A=<silent>') ? 'yes' : "no ($out)", "\n";
?>
--EXPECT--
peer B got the publish over TLS: yes
publisher excluded: yes
