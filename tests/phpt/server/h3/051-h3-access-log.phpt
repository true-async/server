--TEST--
HttpServer access log (#5, B6): h3 request emits an access record with proto=h3
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
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-051';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp) {
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem"); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$logPath = sys_get_temp_dir() . '/php-http-h3-051-' . getmypid() . '.log';
@unlink($logPath);
$fh = fopen($logPath, 'w+b');

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('hi h3')->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(120000);
    $body = shell_exec(sprintf('%s 127.0.0.1 %d /h3path GET 2>/dev/null',
        escapeshellarg($client_bin), $port));
    echo "body: ", trim($body ?? ''), "\n";
    $server->stop();
});

$server->start();
await($client);

fflush($fh); fclose($fh);
$log = file_get_contents($logPath);
@unlink($logPath);

$rec = null;
foreach (explode("\n", trim($log)) as $line) {
    if ($line === '') continue;
    $r = json_decode($line, true);
    if (($r['Attributes']['path'] ?? '') === '/h3path') $rec = $r['Attributes'];
}
echo "rec: ", $rec !== null
    ? sprintf("method=%s status=%d proto=%s bytes=%d remote=%s",
        $rec['method'], $rec['status'], $rec['proto'], $rec['bytes'],
        str_starts_with($rec['remote'] ?? '', '127.0.0.1:') ? 'yes' : 'no')
    : "missing", "\n";
echo "done\n";
?>
--EXPECT--
body: hi h3
rec: method=GET status=200 proto=h3 bytes=5 remote=yes
done
