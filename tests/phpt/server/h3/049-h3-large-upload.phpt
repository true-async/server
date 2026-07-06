--TEST--
HttpServer: HTTP/3 large buffered request body (1 MiB) — inbound flow-control credit
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--FILE--
<?php
/* Regression: nghttp3_conn_read_stream's consumed count EXCLUDES DATA
 * payload (deferred-consume contract), so h3_recv_data_cb must extend the
 * QUIC stream/connection windows itself for buffered body bytes. Before
 * the fix nothing did — an upload larger than the initial stream window
 * (256 KiB default) stalled forever. 1 MiB pins four window refills. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-049';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $req->awaitBody();
    $body = $req->getBody();
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody('len=' . strlen($body) . ';md5=' . md5($body));
});

$py = __DIR__ . '/../grpc/_h3grpc_client.py';

$client = spawn(function () use ($server, $port, $py, $tmp) {
    usleep(200000);

    $payload = random_bytes(1024 * 1024);       /* 1 MiB — 4x the window */
    $hexfile = $tmp . '/body.hex';
    file_put_contents($hexfile, bin2hex($payload));

    $cmd = sprintf("python3 %s 127.0.0.1 %d /upload application/octet-stream @%s 2>/dev/null",
        escapeshellarg($py), $port, escapeshellarg($hexfile));
    $out = shell_exec($cmd) ?? '';
    @unlink($hexfile);

    $body = '';
    if (preg_match('/^BODYHEX ([0-9a-f]*)$/m', $out, $m)) {
        $body = hex2bin($m[1]);
    }

    echo "saw_status_200=", (int)(strpos($out, 'HDR :status: 200') !== false), "\n";
    echo "echo_ok=", (int)($body === 'len=1048576;md5=' . md5($payload)), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
saw_status_200=1
echo_ok=1
Done
