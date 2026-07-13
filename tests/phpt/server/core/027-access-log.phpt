--TEST--
HttpServer access log (#5, B6): per-request record + category routing (h1)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(1);
$pid  = getmypid();
$mk = function (string $tag) use ($pid) {
    $p = sys_get_temp_dir() . "/php-http-027-$tag-$pid.log";
    @unlink($p);
    return [$p, fopen($p, "w+b")];
};

[$accPath, $accFh] = $mk('access');   // category access → per-request only
[$appPath, $appFh] = $mk('app');      // default (app)   → diagnostics only
[$allPath, $allFh] = $mk('all');      // category all    → both

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $accFh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
        ['type' => 'stream', 'stream' => $appFh, 'format' => 'plain',
         'level' => LogSeverity::INFO],
        ['type' => 'stream', 'stream' => $allFh, 'format' => 'logfmt',
         'category' => 'all', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode($req->getPath() === '/miss' ? 404 : 200)
        ->setBody('hello')->end();
});

spawn(function () use ($server, $port) {
    usleep(50000);
    foreach (['/ok?q=1', '/miss'] as $target) {
        $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
        if (!$c) continue;
        fwrite($c, "GET $target HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        while (!feof($c)) { if (@fread($c, 8192) === false) break; }
        fclose($c);
    }
    usleep(50000);
    $server->stop();
});
$server->start();

foreach ([$accFh, $appFh, $allFh] as $fh) { fflush($fh); fclose($fh); }
$acc = file_get_contents($accPath);
$app = file_get_contents($appPath);
$all = file_get_contents($allPath);
@unlink($accPath); @unlink($appPath); @unlink($allPath);

/* Access sink: exactly the two request records, JSON with full attrs. */
$recs = [];
foreach (explode("\n", trim($acc)) as $line) {
    if ($line !== '') $recs[] = json_decode($line, true);
}
echo "access lines: ", count($recs), "\n";
$ok   = null; $miss = null;
foreach ($recs as $r) {
    $a = $r['Attributes'] ?? [];
    if (($a['url.path'] ?? '') === '/ok')   $ok = $a;
    if (($a['url.path'] ?? '') === '/miss') $miss = $a;
}
/* Stable OTel HTTP semconv names; address and port are separate attributes,
 * and the query string is its own attribute rather than glued onto the path. */
echo "ok rec: ", $ok !== null
    ? sprintf("method=%s status=%d proto=%s bytes=%d dur=%s addr=%s port=%s query=%s",
        $ok['http.request.method'], $ok['http.response.status_code'],
        $ok['network.protocol.version'], $ok['http.response.body.size'],
        isset($ok['http.server.request.duration'])
            && $ok['http.server.request.duration'] > 0 ? 'yes' : 'no',
        ($ok['client.address'] ?? '') === '127.0.0.1' ? 'yes' : 'no',
        ($ok['client.port'] ?? 0) > 0 ? 'yes' : 'no',
        $ok['url.query'] ?? '-')
    : "missing", "\n";
echo "miss status: ", $miss['http.response.status_code'] ?? '?', "\n";
echo "access has diag: ", str_contains($acc, 'server.start') ? "yes" : "no", "\n";

/* App sink (default category): diagnostics only. */
echo "app has start: ", str_contains($app, 'server.start') ? "yes" : "no", "\n";
echo "app has access: ", str_contains($app, 'GET /ok') ? "yes" : "no", "\n";

/* 'all' sink: both. */
echo "all has start: ", str_contains($all, 'server.start') ? "yes" : "no", "\n";
echo "all has access: ", str_contains($all, 'url.path=/ok') ? "yes" : "no", "\n";

/* invalid category rejected */
try {
    (new HttpServerConfig())->setLogSinks([
        ['type' => 'stdout', 'category' => 'nope', 'level' => LogSeverity::INFO]]);
    echo "bad-category: accepted\n";
} catch (\Throwable $e) {
    echo "bad-category: rejected\n";
}

echo "Done\n";
--EXPECT--
access lines: 2
ok rec: method=GET status=200 proto=1.1 bytes=5 dur=yes addr=yes port=yes query=q=1
miss status: 404
access has diag: no
app has start: yes
app has access: no
all has start: yes
all has access: yes
bad-category: rejected
Done
