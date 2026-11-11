--TEST--
HttpRequest::isSecure() is true for an HTTP/3 request (#444)
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
/* QUIC always carries TLS 1.3, so every HTTP/3 request is secure; the
 * in-thread stream sets the flag where it copies the peer. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-110';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setBody(($req->isSecure() ? 'secure' : 'plain') . ' ' . $req->getHttpVersion());
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);
    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';
    echo "body=", trim(preg_replace('/^STATUS=\d+\n?/m', '', $out)), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%Abody=secure 3.0
%Adone
