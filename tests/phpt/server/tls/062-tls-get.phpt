--TEST--
HttpServer: GET / over TLS returns handler's 200 response
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-062';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert = $tmp_dir . '/cert.pem';
$key  = $tmp_dir . '/key.pem';
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19940 + getmypid() % 20;

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
        ->setHeader('X-Method', $req->getMethod())
        ->setBody('hello-over-tls')
        ->end();
    $server->stop();
});

$client = spawn(function () use ($port) {
    usleep(50000);
    // -k skips self-signed verification; --http1.1 pins protocol;
    // -s silences progress; -o writes body; -w formats status to stdout.
    $cmd = sprintf(
        'curl -sk --http1.1 -m 4 -o %s -w "HTTP=%%{http_code} CT=%%{content_type} METHOD=%%{header_json}" ' .
        'https://127.0.0.1:%d/ 2>&1',
        escapeshellarg(__DIR__ . '/tmp-062/body.out'),
        $port
    );
    return shell_exec($cmd);
});

// Safety net.
spawn(function () use ($server) {
    usleep(2000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$meta = await($client);
$body = @file_get_contents($tmp_dir . '/body.out');

echo "status: "  . (strpos($meta, 'HTTP=200') !== false ? 'ok' : 'bad') . "\n";
echo "ctype: "   . (stripos($meta, 'text/plain') !== false ? 'ok' : 'bad') . "\n";
echo "xmethod: " . (stripos($meta, '"x-method":["GET"]') !== false ? 'ok' : 'bad') . "\n";
echo "body: "    . ($body === 'hello-over-tls' ? 'ok' : 'bad') . "\n";

@unlink($cert); @unlink($key); @unlink($tmp_dir . '/body.out');
@rmdir($tmp_dir);

echo "Done\n";
--EXPECT--
status: ok
ctype: ok
xmethod: ok
body: ok
Done
