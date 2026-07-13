--TEST--
StaticHandler (#5): the hard-zero serve path emits an access record
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* The static hard-zero FSM bypasses the PHP handler coroutine, so its access
 * record is emitted by the send-file engine, not the coroutine tail. The engine
 * resolves the log state from the server it is handed; a NULL server (the
 * reactor thread) silences it, a real server (h1/h2 worker) logs it. Serving a
 * file over h1 must produce exactly one access record with the served status. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\LogSeverity;
use function Async\spawn;

$root = sys_get_temp_dir() . '/static-acc-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/hello.txt", "hello static");

$log = sys_get_temp_dir() . '/php-http-static-acc-' . getmypid() . '.log';
@unlink($log);
$fh = fopen($log, 'w+b');

register_shutdown_function(function () use ($root, $log) {
    @unlink("$root/hello.txt"); @rmdir($root); @unlink($log);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/static/', $root))->disableIndex());

spawn(function () use ($server, $port) {
    usleep(80000);
    $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    if ($c) {
        fwrite($c, "GET /static/hello.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        while (!feof($c)) { if (@fread($c, 8192) === false) break; }
        fclose($c);
    }
    usleep(150000);
    $server->stop();
});
$server->start();

fflush($fh);
fclose($fh);

$recs = [];
foreach (explode("\n", trim(file_get_contents($log))) as $line) {
    if ($line === '') continue;
    $r = json_decode($line, true);
    if (is_array($r)) $recs[] = $r['Attributes'] ?? [];
}
$hit = null;
foreach ($recs as $a) {
    if (($a['url.path'] ?? '') === '/static/hello.txt') $hit = $a;
}
echo 'records=', count($recs), "\n";
echo 'static_logged=', ($hit !== null ? 1 : 0), "\n";
echo 'status=', ($hit['http.response.status_code'] ?? '?'), "\n";
echo "Done\n";
?>
--EXPECT--
records=1
static_logged=1
status=200
Done
