--TEST--
HttpServer: HTTP/1.1 fallback when client ALPN excludes h2
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/* Negotiation regression guard — our server's preference list opens
 * with "h2" but legacy HTTP/1.1-only clients must still succeed.
 * Drives curl with --http1.1 which restricts its ALPN offer to just
 * "http/1.1"; server must select it and dispatch through the HTTP/1
 * strategy.  */

require_once __DIR__ . '/_h2_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-074';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert = $tmp_dir . '/cert.pem';
$key  = $tmp_dir . '/key.pem';
if (!h2_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$port = 19990 + getmypid() % 9;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('hi-over-h1-tls');
    $server->stop();
});

$client = spawn(function () use ($port) {
    usleep(50000);
    $cmd = sprintf(
        'curl -sk --http1.1 -m 4 -o /dev/null '
        . '-w "HTTP=%%{http_code} V=%%{http_version}" '
        . 'https://127.0.0.1:%d/ 2>&1',
        $port
    );
    return shell_exec($cmd);
});

spawn(function () use ($server) {
    usleep(2000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$meta = await($client);

echo "status: "  . (strpos($meta, 'HTTP=200') !== false ? 'ok' : 'bad') . "\n";
echo "version: " . (strpos($meta, 'V=1.1')    !== false ? 'ok' : 'bad') . "\n";

@unlink($cert); @unlink($key);
@rmdir($tmp_dir);
--EXPECT--
status: ok
version: ok
