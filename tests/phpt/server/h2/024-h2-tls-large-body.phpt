--TEST--
HttpServer: HTTP/2 over TLS — buffered setBody >64K survives WINDOW_UPDATE
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/* Regression: buffered (setBody) data_provider must wake on
 * WINDOW_UPDATE once the initial flow-control window (65535) is
 * exhausted. Prior to the fix, the stream parked after 64K because
 * nothing called http2_session_emit when the peer reopened the window. */

require_once __DIR__ . '/_h2_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-024';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert = $tmp_dir . '/cert.pem';
$key  = $tmp_dir . '/key.pem';
if (!h2_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$size = 256 * 1024;
$body = str_repeat('A', $size);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($server, $body) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'application/octet-stream')
        ->setBody($body);
    $server->stop();
});

$client = spawn(function () use ($port, $size) {
    usleep(80000);
    $out = $tmp_dir = sys_get_temp_dir() . '/h2_024_body.bin';
    @unlink($out);
    $cmd = sprintf(
        'curl -sk --http2 -m 8 -o %s '
        . '-w "HTTP=%%{http_code} V=%%{http_version} SIZE=%%{size_download}" '
        . 'https://127.0.0.1:%d/ 2>&1',
        escapeshellarg($out), $port
    );
    $meta = shell_exec($cmd);
    return [$meta, $out];
});

spawn(function () use ($server) {
    usleep(7000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
[$meta, $out_path] = await($client);

echo "status: "   . (strpos($meta, 'HTTP=200')          !== false ? 'ok' : 'bad') . "\n";
echo "version: "  . (strpos($meta, 'V=2')               !== false ? 'ok' : 'bad') . "\n";
echo "size: "     . (strpos($meta, 'SIZE='.$size)       !== false ? 'ok' : 'bad') . "\n";
echo "content: "  . (is_file($out_path) && filesize($out_path) === $size
                       && hash_file('crc32b', $out_path) === hash('crc32b', $body)
                     ? 'ok' : 'bad') . "\n";

@unlink($out_path);
@unlink($cert); @unlink($key);
@rmdir($tmp_dir);
--EXPECT--
status: ok
version: ok
size: ok
content: ok
