--TEST--
TLS: tls_bytes_ciphertext_out_total counts the ciphertext writes that went out, not one the reactor refused
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'php_ssl' => true]);
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* The handler arms a fault point that submits the response's ciphertext with a
 * length the reactor refuses at submit, so that write never reaches the socket.
 * A counting relay between the client and the server is the oracle: every
 * ciphertext byte the server's kernel accepted on this one connection passed
 * through it, so the counter must equal what the relay forwarded downstream.
 * The inbound side has no such oracle here: the client's close_notify reaches
 * a socket the server has already closed and never reads. */
require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/../_byte_relay.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp = __DIR__ . '/tmp-tls-020';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$client_php = $tmp . '/client.php';
file_put_contents($client_php, <<<'CLIENT'
<?php
$port = (int)$argv[1];
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$fp = @stream_socket_client("ssl://127.0.0.1:$port", $e, $s, 4, STREAM_CLIENT_CONNECT, $ctx);
if (!$fp) { echo "CONNECT_FAIL $s\n"; exit(1); }
stream_set_timeout($fp, 4);
fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
$r = '';
while (!feof($fp)) {
    $c = @fread($fp, 8192);
    if ($c === '' || $c === false) { break; }
    $r .= $c;
}
echo "status: ", strtok($r, "\r\n") ?: '(none)', "\n";
CLIENT
);

$point = 'tls/cipher_write/refused';
$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) use ($point) {
    _http_fault_enable($point);
    $res->setStatusCode(200)->setBody(str_repeat('x', 4000))->end();
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
echo "hits: ", _http_fault_hits($point), "\n";
echo "out equals relay: ", $t['tls_bytes_ciphertext_out_total'] === $relay->down ? 'yes'
    : "no ({$t['tls_bytes_ciphertext_out_total']} counted, {$relay->down} relayed)", "\n";
--EXPECT--
status: (none)
relay closed: yes
hits: 1
out equals relay: yes
