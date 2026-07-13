--TEST--
Access log (#5): http.server.request.duration is present under the worker pool
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Under the reactor pool the worker stamps its own service window on the
 * dispatch ctx, but the access record is built from the request. The window
 * has to be copied onto the request or the duration attribute is dropped, so a
 * pool-mode access log silently loses http.server.request.duration. Every
 * record must carry a positive duration. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(1);
$path = sys_get_temp_dir() . '/php-http-060-' . getmypid() . '.log';
@unlink($path);
register_shutdown_function(fn() => @unlink($path));

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
$server->addHttpHandler(fn($req, $res) => $res->setStatusCode(200)->setBody('ok')->end());

spawn(function () use ($server, $port) {
    usleep(400000);
    for ($i = 0; $i < 4; $i++) {
        $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
        if (!$c) continue;
        fwrite($c, "GET /d$i HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        while (!feof($c)) { if (@fread($c, 8192) === false) break; }
        fclose($c);
    }
    usleep(300000);
    $server->stop();
});
$server->start();

$log = @file_get_contents($path) ?: '';

$records = 0; $with_dur = 0;
foreach (explode("\n", trim($log)) as $line) {
    if ($line === '') continue;
    $r = json_decode($line, true);
    if (!is_array($r) || !str_starts_with($r['Attributes']['url.path'] ?? '', '/d')) continue;
    $records++;
    $d = $r['Attributes']['http.server.request.duration'] ?? 0;
    if (is_numeric($d) && $d > 0) $with_dur++;
}
echo 'records=',   $records, "\n";
echo 'with_duration=', $with_dur, "\n";
echo "Done\n";
?>
--EXPECTF--
records=4
with_duration=4
Done%A
