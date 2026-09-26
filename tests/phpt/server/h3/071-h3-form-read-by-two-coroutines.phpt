--TEST--
HttpServer: HTTP/3 form read by two coroutines at once is parsed once
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--INI--
upload_tmp_dir={PWD}/tmp-071/uploads
--FILE--
<?php
/* An HTTP/3 multipart body is parsed in slices with a yield between them. A
 * second getter arriving during that yield waits for the first one's form;
 * it used to start a second parse, and whichever finished last replaced the
 * other's form and processor, which then leaked with their temp files. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-071';
@mkdir("$tmp/uploads", 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
$rc   = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$boundary = 'h3bnd';
$body     = "$tmp/body.bin";
file_put_contents($body,
    "--$boundary\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\nbefore\r\n"
    . "--$boundary\r\nContent-Disposition: form-data; name=\"big\"; filename=\"big.bin\"\r\n"
    . "Content-Type: application/octet-stream\r\n\r\n" . str_repeat('z', 400 * 1024) . "\r\n"
    . "--$boundary--\r\n");

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key));

$server->addHttpHandler(function ($req, $res) {
    $read = static fn() => [$req->getPost(), array_map(
        static fn($file) => $file->getClientFilename() . ':' . $file->getSize(), $req->getFiles())];

    $first  = spawn($read);
    $second = spawn($read);
    $a = await($first);
    $b = await($second);

    $res->setStatusCode(200)->setBody(json_encode($a) . ($a === $b ? ' same' : ' DIFFERENT ' . json_encode($b)));
});

$client = spawn(function () use ($server, $port, $body, $boundary) {
    usleep(80000);
    $out = shell_exec(sprintf('H3CLIENT_HEADER=%s %s 127.0.0.1 %d /form POST %s 2>&1',
        escapeshellarg("content-type: multipart/form-data; boundary=$boundary"),
        escapeshellarg(__DIR__ . '/../../../h3client/h3client'), $port, escapeshellarg($body))) ?? '';

    echo trim(preg_replace('/^STATUS=\d+\n?/m', '', $out)), "\n";
    $server->stop();
});

$server->start();
await($client);

echo 'uploads left: ', count(array_diff(scandir("$tmp/uploads"), ['.', '..'])), "\n";

array_map('unlink', glob("$tmp/uploads/*"));
@rmdir("$tmp/uploads");
@unlink($cert); @unlink($key); @unlink($body); @rmdir($tmp);
?>
--EXPECT--
[{"note":"before"},{"big":"big.bin:409600"}] same
uploads left: 0
