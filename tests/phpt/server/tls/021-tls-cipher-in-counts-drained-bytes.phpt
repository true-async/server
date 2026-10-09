--TEST--
TLS: tls_bytes_ciphertext_in_total counts the bytes a refused upload's drain reads and drops
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'php_ssl' => true]);
?>
--FILE--
<?php
/* The server answers a 12 MiB upload against a 64 KiB limit with 413 and drains
 * the rest of the body before the close, as tls/017 shows. The drained bytes
 * crossed the socket, so the inbound counter must equal what a counting relay
 * forwarded to the server once the client has finished, and the outbound
 * counter what it forwarded back. */
require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/../_byte_relay.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp = __DIR__ . '/tmp-tls-021';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$client_php = $tmp . '/upload_client.php';
file_put_contents($client_php, <<<'CLIENT'
<?php
$port = (int)$argv[1];
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$fp = @stream_socket_client("ssl://127.0.0.1:$port", $e, $s, 4, STREAM_CLIENT_CONNECT, $ctx);
if (!$fp) { echo "CONNECT_FAIL $s\n"; exit(1); }
stream_set_timeout($fp, 4);
fwrite($fp, "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
$chunk = sprintf("%x\r\n", 65536) . str_repeat('A', 65536) . "\r\n";
$wrote = 0;
for ($i = 0; $i < 192; $i++) {
    $w = @fwrite($fp, $chunk);
    if ($w === false || $w === 0) { break; }
    $wrote++;
}
@fwrite($fp, "0\r\n\r\n");
$r = '';
while (!feof($fp)) {
    $c = @fread($fp, 8192);
    if ($c === '' || $c === false) { break; }
    $r .= $c;
}
fclose($fp);
echo "client wrote the body: ", $wrote === 192 ? 'all' : "$wrote of 192", "\n";
echo "status: ", strtok($r, "\r\n") ?: '(empty)', "\n";
CLIENT
);

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(8)
    ->setWriteTimeout(8)
    ->setMaxBodySize(65536));

$server->addHttpHandler(function ($req, $res) {
    $req->awaitBody();
    $res->setStatusCode(200)->setBody('handler ran past awaitBody')->end();
});

$client = spawn(function () use ($port, $client_php, $server) {
    $relay = new TasByteRelay($port);
    delay(80);
    $out = trim((string) shell_exec(escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($client_php)
        . " {$relay->port} 2>&1"));
    $closed = $relay->waitClosed();
    delay(50);
    $t = $server->getTelemetry();
    $server->stop();
    $relay->stop();

    return [$out, $closed, $relay, $t];
});

$server->start();
[$out, $closed, $relay, $t] = await($client);

@unlink($cert); @unlink($key); @unlink($client_php); @rmdir($tmp);

echo $out, "\n";
echo "relay closed: ", $closed ? 'yes' : 'no', "\n";
echo "in equals relay: ", $t['tls_bytes_ciphertext_in_total'] === $relay->up ? 'yes'
    : "no ({$t['tls_bytes_ciphertext_in_total']} counted, {$relay->up} relayed)", "\n";
echo "out equals relay: ", $t['tls_bytes_ciphertext_out_total'] === $relay->down ? 'yes'
    : "no ({$t['tls_bytes_ciphertext_out_total']} counted, {$relay->down} relayed)", "\n";
--EXPECTF--
client wrote the body: all
status: HTTP/1.1 413%a
relay closed: yes
in equals relay: yes
out equals relay: yes
