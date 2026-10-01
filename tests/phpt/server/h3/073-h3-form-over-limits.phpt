--TEST--
HttpServer: HTTP/3 refuses a form past the body limit or past max_input_vars through the getter
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--INI--
max_input_vars=5
--FILE--
<?php
/* HTTP/3 starts the handler at the end of the headers and buffers the body.
 * A body past setMaxBodySize() used to be finalized as complete when the
 * stream was reset, so the getter parsed what had arrived and the handler got
 * a shortened form; a buffered body was held to a fixed 16 MiB rather than
 * to setMaxBodySize(). The getter throws HttpException 413 now, and 400 for a
 * form past max_input_vars. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-073';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$boundary = 'h3bnd';
$part = static fn(string $name, string $value) =>
    "--$boundary\r\nContent-Disposition: form-data; name=\"$name\"\r\n\r\n$value\r\n";

file_put_contents("$tmp/big.bin", $part('title', 'x') . $part('pad', str_repeat('p', 200 * 1024))
    . $part('amount', '1000') . "--$boundary--\r\n");
file_put_contents("$tmp/many.bin", implode('', array_map(
    static fn($i) => $part("k$i", 'v'), range(1, 6))) . "--$boundary--\r\n");

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setMaxBodySize(64 * 1024)
    ->setCertificate($cert)->setPrivateKey($key));

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    try {
        $seen[] = 'form ' . json_encode(array_keys($req->getPost()));
    } catch (TrueAsync\HttpException $e) {
        $seen[] = 'HttpException ' . $e->getCode();
    }

    $res->setStatusCode(200)->setBody('done');
});

$client = spawn(function () use ($server, $port, $tmp, $boundary) {
    usleep(80000);

    foreach (['big.bin', 'many.bin'] as $file) {
        shell_exec(sprintf('H3CLIENT_HEADER=%s %s 127.0.0.1 %d /form POST %s 2>&1',
            escapeshellarg("content-type: multipart/form-data; boundary=$boundary"),
            escapeshellarg(__DIR__ . '/../../../h3client/h3client'), $port, escapeshellarg("$tmp/$file")));
    }

    $server->stop();
});

$server->start();
await($client);

echo implode("\n", $seen), "\n";

@unlink($cert); @unlink($key); @unlink("$tmp/big.bin"); @unlink("$tmp/many.bin"); @rmdir($tmp);
?>
--EXPECT--
HttpException 413
HttpException 400
