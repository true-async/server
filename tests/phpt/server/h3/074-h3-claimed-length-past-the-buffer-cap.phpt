--TEST--
HttpServer: HTTP/3 does not pre-size a buffer past HTTP3_MAX_BODY_BYTES from a claimed Content-Length
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--INI--
memory_limit=128M
--FILE--
<?php
/* The buffered body is pre-sized from the peer's Content-Length. With a
 * 1 GiB setMaxBodySize() a claim of 900 MB exhausted memory_limit and the
 * bailout took the server down; the buffer cap stays HTTP3_MAX_BODY_BYTES
 * whatever the configured limit is. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-074';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
file_put_contents("$tmp/small.bin", 'tiny body!');

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setMaxBodySize(1024 * 1024 * 1024)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key));

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($server, $port, $tmp) {
    usleep(80000);
    $h3client = escapeshellarg(__DIR__ . '/../../../h3client/h3client');

    shell_exec(sprintf('H3CLIENT_DEADLINE_MS=4000 H3CLIENT_HEADER=%s %s 127.0.0.1 %d /up POST %s 2>&1',
        escapeshellarg('content-length: 900000000'), $h3client, $port, escapeshellarg("$tmp/small.bin")));

    $out = (string)shell_exec(sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d /again GET 2>&1', $h3client, $port));
    echo 'second request: ', preg_match('/^STATUS=(\d+)$/m', $out, $m) ? $m[1] : trim($out), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "server survived\n";

@unlink($cert); @unlink($key); @unlink("$tmp/small.bin"); @rmdir($tmp);
?>
--EXPECT--
second request: 200
server survived
