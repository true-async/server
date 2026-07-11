--TEST--
HttpServer 'file' log sink (#5): path reopened per worker — works under a pool
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
$path = sys_get_temp_dir() . '/php-http-028-' . getmypid() . '.log';
@unlink($path);

/* Invalid spec first. */
try {
    (new HttpServerConfig())->setLogSinks([
        ['type' => 'file', 'level' => LogSeverity::INFO]]);
    echo "no-path: accepted\n";
} catch (\Throwable $e) {
    echo "no-path: rejected\n";
}

/* Worker pool + 'file' access sink: each worker reopens the path itself —
 * the setup where a parent-opened 'stream' resource cannot work. */
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(2)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'file', 'path' => $path, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('ok')->end(); });

spawn(function () use ($server, $port) {
    usleep(400000);   /* pool spin-up */
    for ($i = 0; $i < 4; $i++) {
        $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
        if (!$c) continue;
        fwrite($c, "GET /w$i HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        while (!feof($c)) { if (@fread($c, 8192) === false) break; }
        fclose($c);
    }
    usleep(300000);   /* flush timers */
    $server->stop();
});
$server->start();

$log = @file_get_contents($path) ?: '';
@unlink($path);

$n = 0;
foreach (explode("\n", trim($log)) as $line) {
    if ($line === '') continue;
    $r = json_decode($line, true);
    if (is_array($r) && str_starts_with($r['Attributes']['path'] ?? '', '/w')) $n++;
}
echo "access records: ", $n, "\n";
echo "Done\n";
--EXPECTF--
no-path: rejected
access records: 4
Done%A
