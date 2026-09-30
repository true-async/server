--TEST--
HttpServer syslog sink (#5, B5): RFC 5424 records delivered over a TCP transport
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

/* The sink's port is the one the kernel gives its listener, and the server's
 * is probed apart from it: Windows hands ephemeral ports out in order, so the
 * port after one just taken is the next another test gets. */
$listener = stream_socket_server("tcp://127.0.0.1:0", $errno, $errstr);
if (!$listener) { echo "FAIL: listener $errstr\n"; exit(1); }
$sysName  = stream_socket_get_name($listener, false);
$sysPort  = (int) substr($sysName, strrpos($sysName, ':') + 1);
$httpPort = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $httpPort)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'syslog', 'target' => "tcp://127.0.0.1:$sysPort",
         'facility' => 'local0', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });

/* Stops the server once the first record has arrived whole, then reads on to
 * the EOF the sink's close sends; the 5 s read timeout bounds a silent sink. */
$reader = spawn(function () use ($listener, $server) {
    $received = '';
    $conn = @stream_socket_accept($listener, 5);
    if (!$conn) { $server->stop(); return $received; }
    stream_set_timeout($conn, 5);
    $stopped = false;
    while (!feof($conn)) {
        $chunk = @fread($conn, 8192);
        if ($chunk === false || $chunk === '') { break; }
        $received .= $chunk;
        if (!$stopped && preg_match('/^(\d+) /', $received, $m)
            && strlen($received) >= strlen($m[0]) + (int) $m[1]) {
            $stopped = true;
            $server->stop();
        }
    }
    fclose($conn);
    if (!$stopped) { $server->stop(); }
    return $received;
});

$server->start();
$received = await($reader);

fclose($listener);

/* Octet frame: "LEN SP MSG". local0(16)*8 + INFO(6) = PRI 134. */
if (preg_match('/^(\d+) /', $received, $m)) {
    $len = (int) $m[1];
    $msg = substr($received, strlen($m[0]), $len);
    echo "framelen: ", (strlen($msg) === $len ? "ok" : "bad"), "\n";
    echo "pri: ", (str_starts_with($msg, '<134>1 ') ? "ok" : "bad"), "\n";
} else {
    echo "framelen: none\npri: none\n";
}
echo "has-start: ", (str_contains($received, 'server.start') ? "yes" : "no"), "\n";

echo "done\n";
?>
--EXPECT--
framelen: ok
pri: ok
has-start: yes
done
