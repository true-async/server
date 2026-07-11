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

require_once __DIR__ . '/../_free_port.inc';

$base     = tas_free_port_span(2);
$sysPort  = $base;
$httpPort = $base + 1;

$listener = stream_socket_server("tcp://127.0.0.1:$sysPort", $errno, $errstr);
if (!$listener) { echo "FAIL: listener $errstr\n"; exit(1); }

$received = '';
spawn(function () use ($listener, &$received) {
    $conn = @stream_socket_accept($listener, 5);
    if (!$conn) { return; }
    while (!feof($conn)) {
        $chunk = @fread($conn, 8192);
        if ($chunk === false || $chunk === '') { break; }
        $received .= $chunk;
    }
    fclose($conn);
});

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

spawn(function () use ($server) { usleep(150000); $server->stop(); });
$server->start();

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
