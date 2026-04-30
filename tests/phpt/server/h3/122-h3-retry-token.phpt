--TEST--
HttpServer: HTTP/3 Retry token round-trip (RFC 9000 §8.1.2 source-address validation)
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
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-122';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20420 + getmypid() % 30;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$captured_stats = [];
$server->addHttpHandler(function ($req, $res) use ($server, &$captured_stats) {
    $res->setStatusCode(200)->setBody('retry-ok');
    $captured_stats = $server->getHttp3Stats()[0] ?? [];
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);
    $cmd = sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';
    $server->stop();
    return $out;
});

$server->start();
$out = await($client);

$s = $captured_stats;

echo "body_in_response: ", (str_contains($out, 'retry-ok') ? 'yes' : 'no'), "\n";
echo "retry_sent_at_least_1: ",
    ((int)($s['quic_retry_sent'] ?? 0) >= 1 ? 'yes' : 'no'), "\n";
echo "retry_token_ok_at_least_1: ",
    ((int)($s['quic_retry_token_ok'] ?? 0) >= 1 ? 'yes' : 'no'), "\n";
echo "retry_token_invalid_zero: ",
    ((int)($s['quic_retry_token_invalid'] ?? 0) === 0 ? 'yes' : 'no'), "\n";

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
--EXPECT--
body_in_response: yes
retry_sent_at_least_1: yes
retry_token_ok_at_least_1: yes
retry_token_invalid_zero: yes
done
