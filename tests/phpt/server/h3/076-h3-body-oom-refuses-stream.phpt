--TEST--
HttpServer: HTTP/3 refuses a body that will not fit in memory_limit, and the server keeps going
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--INI--
memory_limit=12M
display_errors=0
--FILE--
<?php
/* The buffered body is pre-sized from the peer's Content-Length, and a claim
 * under setMaxBodySize() but over memory_limit bails out of the allocation.
 * Without a firewall around the append the longjmp crosses the nghttp3 and
 * ngtcp2 frames and the server dies ("The event loop must be stopped"); with
 * it the stream alone is refused. HTTP/2 holds the same line in 062. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-076';
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
    ->setMaxBodySize(16 * 1024 * 1024)
    ->setCertificate($cert)->setPrivateKey($key));

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($server, $port, $tmp) {
    usleep(80000);
    $h3client = escapeshellarg(__DIR__ . '/../../../h3client/h3client');

    shell_exec(sprintf('H3CLIENT_DEADLINE_MS=4000 H3CLIENT_HEADER=%s %s 127.0.0.1 %d /up POST %s 2>&1',
        escapeshellarg('content-length: 15000000'), $h3client, $port, escapeshellarg("$tmp/small.bin")));

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
