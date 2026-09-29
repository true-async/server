--TEST--
TLS: a refused upload is drained before the close — the client finishes its body and reads the 413
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
require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp = __DIR__ . '/tmp-tls-017';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

/* Out of process: the client's TLS handshake is synchronous and would block
 * the server loop. No sleep or clock call anywhere in this section: run-tests
 * retries a FILE section that names one and reports only the retry.
 *
 * 12 MiB against a 64 KiB limit: the server refuses while most of the body is
 * unsent, past what the two socket buffers hold. A close with those bytes
 * unread resets the connection, which fails the rest of the upload here and,
 * on Windows, discards the 413 as well. */
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
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(8)
    ->setWriteTimeout(8)
    ->setMaxBodySize(65536);

$server = new HttpServer($config);

$handlerRanBody = false;
$server->addHttpHandler(function ($req, $res) use (&$handlerRanBody) {
    $req->awaitBody();
    $handlerRanBody = true;
    $res->setStatusCode(200)->setBody('handler ran past awaitBody')->end();
});

$client = spawn(function () use ($port, $client_php, $server) {
    delay(80);
    $out = shell_exec(escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($client_php) . " $port 2>&1");
    $server->stop();

    return trim((string) $out);
});

$server->start();
$out = await($client);

@unlink($cert); @unlink($key); @unlink($client_php); @rmdir($tmp);

echo $out, "\n";
echo "handler ran past awaitBody: ", $handlerRanBody ? 'yes' : 'no', "\n";
echo "Done\n";
--EXPECTF--
client wrote the body: all
status: HTTP/1.1 413%a
handler ran past awaitBody: no
Done
