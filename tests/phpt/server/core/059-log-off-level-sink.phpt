--TEST--
setLogSinks (#5): a level=OFF sink stays disabled under a worker pool
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A sink declared with level=OFF logs nothing. In single-thread mode the sink
 * is dropped at start; the pool path freezes each sink's level into the worker
 * snapshot, and an OFF level must round-trip as OFF rather than collapsing to
 * the INFO default and reviving a sink the user turned off. The OFF file must
 * stay empty while the INFO file collects the records. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(1);
$offPath = sys_get_temp_dir() . '/php-http-059-off-'  . getmypid() . '.log';
$onPath  = sys_get_temp_dir() . '/php-http-059-on-'   . getmypid() . '.log';
@unlink($offPath); @unlink($onPath);

register_shutdown_function(function () use ($offPath, $onPath) {
    @unlink($offPath); @unlink($onPath);
});

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(2)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'file', 'path' => $offPath, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::OFF],
        ['type' => 'file', 'path' => $onPath, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(fn($req, $res) => $res->setStatusCode(200)->setBody('ok')->end());

spawn(function () use ($server, $port) {
    usleep(400000);
    for ($i = 0; $i < 4; $i++) {
        $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
        if (!$c) continue;
        fwrite($c, "GET /w$i HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        while (!feof($c)) { if (@fread($c, 8192) === false) break; }
        fclose($c);
    }
    usleep(300000);
    $server->stop();
});
$server->start();

$off = @file_get_contents($offPath) ?: '';
$on  = @file_get_contents($onPath)  ?: '';

$count = fn($s) => count(array_filter(explode("\n", trim($s)), fn($l) => $l !== ''));

echo 'off_records=', $count($off), "\n";
echo 'on_records=',  ($count($on) === 4 ? 4 : $count($on)), "\n";
echo "Done\n";
?>
--EXPECTF--
off_records=0
on_records=4
Done%A
