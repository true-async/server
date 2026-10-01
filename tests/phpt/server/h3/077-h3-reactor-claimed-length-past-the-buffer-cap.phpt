--TEST--
HttpServer: HTTP/3 through the reactor pool does not pre-size a body past the buffer cap from a claimed Content-Length
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* A reactor thread builds the body in persistent memory, pre-sized from the
 * peer's Content-Length, and a persistent allocation that fails ends the
 * process ("Out of memory"). The claim here is 200 TB, past the 128 TiB of
 * user address space on x86-64, so malloc refuses it even under
 * vm.overcommit_memory=1; under the default heuristic any claim past RAM and
 * swap does the same. The pre-size stops at the buffer cap, as the buffered
 * path of a worker does. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-077';
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
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2));

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($server, $port, $tmp) {
    usleep(600000);
    $h3client = escapeshellarg(__DIR__ . '/../../../h3client/h3client');

    shell_exec(sprintf('H3CLIENT_DEADLINE_MS=4000 H3CLIENT_HEADER=%s %s 127.0.0.1 %d /up POST %s 2>&1',
        escapeshellarg('content-length: 200000000000000'), $h3client, $port, escapeshellarg("$tmp/small.bin")));

    $out = (string)shell_exec(sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d /again GET 2>&1', $h3client, $port));
    echo 'second request: ', preg_match('/^STATUS=(\d+)$/m', $out, $m) ? $m[1] : trim($out), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "server survived\n";

@unlink($cert); @unlink($key); @unlink("$tmp/small.bin"); @rmdir($tmp);
?>
--EXPECTF--
%Asecond request: 200
%Aserver survived
