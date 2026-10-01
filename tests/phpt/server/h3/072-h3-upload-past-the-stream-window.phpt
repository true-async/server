--TEST--
HttpServer: HTTP/3 upload of 4 MiB at the default 256 KiB stream window arrives whole
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* The body is sixteen stream windows long, so both sides have to move the
 * window along: the server by crediting what it read, the client by sending
 * again once MAX_STREAM_DATA arrives. The client used to block the stream on
 * the first STREAM_DATA_BLOCKED and never unblock it, and the upload hung. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-072';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
$body_path = "$tmp/body.bin";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$body = '';
for ($i = 0; $i < 4 * 1024 * 1024 / 16; $i++) {
    $body .= sprintf('%015x|', $i);
}
file_put_contents($body_path, $body);
$expected = sprintf('len=%d sha1=%s', strlen($body), sha1($body));

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key));

$server->addHttpHandler(function ($req, $res) {
    $req->awaitBody();
    $received = $req->getBody();
    $res->setStatusCode(200)->setBody(sprintf('len=%d sha1=%s', strlen($received), sha1($received)));
});

$client = spawn(function () use ($server, $port, $body_path, $expected) {
    usleep(80000);
    $out = shell_exec(sprintf('%s 127.0.0.1 %d /upload POST %s 2>&1',
        escapeshellarg(__DIR__ . '/../../../h3client/h3client'), $port, escapeshellarg($body_path))) ?? '';
    $got = trim(preg_replace('/^STATUS=\d+\n?/m', '', $out));

    echo $got === $expected ? 'whole body received' : "got: $got", "\n";
    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @unlink($body_path); @rmdir($tmp);
?>
--EXPECT--
whole body received
