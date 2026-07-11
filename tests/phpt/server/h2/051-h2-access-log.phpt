--TEST--
HttpServer access log (#5, B6): h2 request emits an access record with network.protocol.version=2
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$path = sys_get_temp_dir() . '/php-http-h2-051-' . getmypid() . '.log';
@unlink($path);
$fh = fopen($path, 'w+b');

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('hi h2')->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    exec(sprintf(
        'curl --http2-prior-knowledge -sS --max-time 3 http://127.0.0.1:%d/h2path 2>&1',
        $port), $out, $rc);
    echo "curl_rc=$rc\n";
    $server->stop();
});

$server->start();
await($client);

fflush($fh); fclose($fh);
$log = file_get_contents($path);
@unlink($path);

$rec = null;
foreach (explode("\n", trim($log)) as $line) {
    if ($line === '') continue;
    $r = json_decode($line, true);
    if (($r['Attributes']['url.path'] ?? '') === '/h2path') $rec = $r['Attributes'];
}
echo "rec: ", $rec !== null
    ? sprintf("method=%s status=%d proto=%s bytes=%d addr=%s",
        $rec['http.request.method'], $rec['http.response.status_code'],
        $rec['network.protocol.version'], $rec['http.response.body.size'],
        ($rec['client.address'] ?? '') === '127.0.0.1' ? 'yes' : 'no')
    : "missing", "\n";
echo "Done\n";
--EXPECT--
curl_rc=0
rec: method=GET status=200 proto=2 bytes=5 addr=yes
Done
