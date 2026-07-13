--TEST--
Access log (#5): an oversized record truncates without swallowing the next line
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* A record that overflows the formatter buffer is truncated. On a plain stream
 * sink the trailing '\n' is the only record separator, so losing it would merge
 * the truncated record with the next one — a client could hide an unrelated
 * request behind a long URL. The oversized record must still end in a newline,
 * so the following record lands on its own line and stays valid JSON. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$log  = sys_get_temp_dir() . '/php-http-058-' . getmypid() . '.log';
@unlink($log);
$fh = fopen($log, 'w+b');

register_shutdown_function(fn() => @unlink($log));

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'stream', 'stream' => $fh, 'format' => 'json',
         'category' => 'access', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(fn($req, $res) => $res->setStatusCode(200)->setBody('ok')->end());

spawn(function () use ($server, $port) {
    usleep(80000);
    /* short, a ~3 KiB query (past the 2 KiB formatter buffer), short. */
    $targets = ['/first', '/long?q=' . str_repeat('A', 3000), '/last'];
    $c = @stream_socket_client("tcp://127.0.0.1:$port", $e1, $e2, 2);
    if ($c) {
        foreach ($targets as $t) {
            fwrite($c, "GET $t HTTP/1.1\r\nHost: x\r\n\r\n");
            usleep(30000);
            @fread($c, 65536);
        }
        fclose($c);
    }
    usleep(120000);
    $server->stop();
});
$server->start();

fflush($fh);
fclose($fh);

$lines = array_values(array_filter(explode("\n", rtrim(file_get_contents($log), "\n")),
    fn($l) => $l !== ''));

echo 'lines=', count($lines), "\n";              // three records, none merged
/* The record after the oversized one must be intact + valid JSON. */
$last = json_decode($lines[count($lines) - 1] ?? '', true);
echo 'last_valid=', (is_array($last) ? 1 : 0), "\n";
echo 'last_path=', ($last['Attributes']['url.path'] ?? '?'), "\n";
echo "done\n";
?>
--EXPECT--
lines=3
last_valid=1
last_path=/last
done
