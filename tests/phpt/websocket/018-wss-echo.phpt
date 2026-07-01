--TEST--
WebSocket: wss:// (WebSocket over TLS) handshake + echo round-trip
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
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-wss';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

// Separate-process wss:// client (out-of-process so its synchronous TLS
// handshake doesn't block the in-process server loop). Written at runtime
// because tests/phpt/**/*.php is gitignored (run-tests artifact namespace).
$client_php = $tmp . '/wss_client.php';
file_put_contents($client_php, <<<'CLIENT'
<?php
$host = $argv[1]; $port = (int)$argv[2]; $msg = $argv[3] ?? 'hello-wss';
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$fp = @stream_socket_client("ssl://$host:$port", $e, $s, 4, STREAM_CLIENT_CONNECT, $ctx);
if (!$fp) { echo "CONNECT_FAIL $s\n"; exit(1); }
stream_set_timeout($fp, 4);
fwrite($fp, "GET / HTTP/1.1\r\nHost: $host\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
$hs = '';
while (!str_contains($hs, "\r\n\r\n")) { $c = fread($fp, 1); if ($c === '' || $c === false) break; $hs .= $c; }
$status = str_contains($hs, ' 101 ') ? '101' : 'no101';
$mask = random_bytes(4); $n = strlen($msg); $m = '';
for ($i = 0; $i < $n; $i++) { $m .= chr(ord($msg[$i]) ^ ord($mask[$i & 3])); }
fwrite($fp, chr(0x81) . chr(0x80 | $n) . $mask . $m);
$h = fread($fp, 2);
if (strlen($h) < 2) { echo "$status NO_FRAME\n"; exit(1); }
$op = ord($h[0]) & 0x0f; $len = ord($h[1]) & 0x7f; $p = '';
while (strlen($p) < $len) { $c = fread($fp, $len - strlen($p)); if ($c === '' || $c === false) break; $p .= $c; }
fclose($fp);
echo "$status op=0x", dechex($op), " payload=$p\n";
CLIENT
);

$port = 19920 + getmypid() % 30;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($m = $ws->recv()) !== null) {
        $ws->send('echo:' . $m->data);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $client_php) {
    usleep(80000);
    $cmd = escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($client_php)
         . " 127.0.0.1 $port hello-wss 2>&1";
    return shell_exec($cmd);
});

$stopper = spawn(function () use ($server, $client) {
    await($client);
    usleep(30000);
    if ($server->isRunning()) { $server->stop(); }
});

// Safety net.
spawn(function () use ($server) {
    usleep(3000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$out = trim((string) await($client));
await($stopper);

@unlink($cert); @unlink($key); @unlink($client_php); @rmdir($tmp);

echo "client: $out\n";
echo "handshake 101: ", (str_contains($out, '101') ? 'yes' : 'no'), "\n";
echo "text opcode:  ", (str_contains($out, 'op=0x1') ? 'yes' : 'no'), "\n";
echo "echo payload: ", (str_contains($out, 'payload=echo:hello-wss') ? 'yes' : 'no'), "\n";
echo "Done\n";
--EXPECT--
client: 101 op=0x1 payload=echo:hello-wss
handshake 101: yes
text opcode:  yes
echo payload: yes
Done
