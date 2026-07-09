--TEST--
HttpServer: HTTP/3 sendFile() delivered through the reactor/worker split (#105)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Regression for #105: under TRUE_ASYNC_SERVER_REACTOR_POOL=1 a handler
 * calling $res->sendFile() used to be silently dropped — the worker render
 * path had no send-file case. Now the worker marshals path + options to the
 * reactor as a SEND_FILE wire and the reactor opens the file and runs the
 * shared sendfile engine. Assert byte integrity across the 16 KiB chunk
 * boundary so a truncation/offset regression in the split shows up. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-049';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$root = $tmp . '/root';
@mkdir($root, 0700, true);
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$sizes = [1, 16384, 50000];
$sha = [];
foreach ($sizes as $n) {
    $buf = '';
    for ($i = 0; $i < $n; $i++) $buf .= chr(($i * 31 + 7) & 0xff);
    file_put_contents("$root/f$n.bin", $buf);
    $sha[$n] = sha1($buf);
}

register_shutdown_function(function () use ($tmp, $root, $sizes, $cert, $key) {
    foreach ($sizes as $n) @unlink("$root/f$n.bin");
    @unlink($cert); @unlink($key); @rmdir($root); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($root) {
    $name = basename($req->getUri());
    $res->sendFile($root . '/' . $name);
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $sizes, $sha) {
    usleep(600000);

    foreach ($sizes as $n) {
        $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d /f%d.bin GET 2>/dev/null',
            escapeshellarg($client_bin), $port, $n);
        $body = shell_exec($cmd) ?? '';
        $ok = (strlen($body) === $n) && (sha1($body) === $sha[$n]);
        printf("size=%d len=%d match=%d\n", $n, strlen($body), $ok ? 1 : 0);
    }

    /* Pool-parent stop() is unsupported (#11); the delivery assertions above
     * are the point. The trailing %A absorbs its exception + shutdown noise. */
    $server->stop();
});

$server->start();
?>
--EXPECTF--
%Asize=1 len=1 match=1
size=16384 len=16384 match=1
size=50000 len=50000 match=1
%A
