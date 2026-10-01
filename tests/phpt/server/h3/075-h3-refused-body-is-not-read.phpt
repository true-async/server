--TEST--
HttpServer: an HTTP/3 body refused past setMaxBodySize() is not handed out as a whole one
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
/* HTTP/3 finalizes a refused stream so a waiting handler wakes. The part that
 * arrived then looked like a complete body: readBody() and getBody() returned
 * it. Every read of a refused body throws HttpException 413. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-075';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
file_put_contents("$tmp/body.bin", str_repeat('b', 200 * 1024));

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setMaxBodySize(64 * 1024)
    ->setCertificate($cert)->setPrivateKey($key));

$seen = [];

$server->addHttpHandler(function ($req, $res) use (&$seen) {
    foreach (['awaitBody', 'readBody', 'readBody', 'getBody', 'hasBody'] as $read) {
        try {
            $value = $req->$read();
            $seen[] = "$read: " . (is_string($value) ? strlen($value) . ' bytes' : var_export($value, true));
        } catch (TrueAsync\HttpException $e) {
            $seen[] = "$read: HttpException " . $e->getCode();
        }
    }

    $res->setStatusCode(200)->setBody('done');
});

$client = spawn(function () use ($server, $port, $tmp) {
    usleep(80000);
    shell_exec(sprintf('%s 127.0.0.1 %d /up POST %s 2>&1',
        escapeshellarg(__DIR__ . '/../../../h3client/h3client'), $port, escapeshellarg("$tmp/body.bin")));
    $server->stop();
});

$server->start();
await($client);

echo implode("\n", $seen), "\n";

@unlink($cert); @unlink($key); @unlink("$tmp/body.bin"); @rmdir($tmp);
?>
--EXPECT--
awaitBody: HttpException 413
readBody: HttpException 413
readBody: HttpException 413
getBody: HttpException 413
hasBody: HttpException 413
