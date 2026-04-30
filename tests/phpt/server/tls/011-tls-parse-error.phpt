--TEST--
HttpServer: parse-error 414 over TLS reaches the client through the encrypted channel
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'php_ssl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-069';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert = $tmp_dir . '/cert.pem';
$key  = $tmp_dir . '/key.pem';
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

$port = 19960 + getmypid() % 20;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    // Should NEVER fire — parse error before dispatch.
    $res->setStatusCode(200)->setBody('should-not-run')->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(80000);
    // A 9 KB URI cannot go on a shell command line: cmd.exe caps at ~8191 chars
    // and stream_socket_client("ssl://") inside the same fiber deadlocks (SSL
    // handshake blocks the event loop so the server cannot respond).
    // _tls_request.php runs as a separate PHP process: short command line,
    // independent TLS stack.
    $php = PHP_BINARY;
    $helper = __DIR__ . '/_tls_request.inc';
    $cmd = sprintf('%s %s 127.0.0.1 %d GET A 9216 2>&1',
        escapeshellarg($php), escapeshellarg($helper), $port);
    $response = (string) shell_exec($cmd);

    preg_match('#^HTTP/[\d.]+ (\d+)#', $response, $m);
    $code = $m[1] ?? '0';
    // Server may use bare \n or \r\n as line endings — handle both.
    $parts = preg_split('/\r?\n\r?\n/', $response, 2);
    $body  = isset($parts[1]) ? trim($parts[1]) : '';

    echo "status: " . ($code === '414' ? 'ok' : 'bad=' . $code) . "\n";
    echo "body: "   . ($body === 'URI Too Long' ? 'ok' : 'bad=' . $body) . "\n";

    $tel = $server->getTelemetry();
    echo "tel-414=" . $tel['parse_errors_414_total'] . "\n";

    $server->stop();
});

spawn(function () use ($server) {
    usleep(3000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key);
@rmdir($tmp_dir);

echo "done\n";
--EXPECT--
status: ok
body: ok
tel-414=1
done
