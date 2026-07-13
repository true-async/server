--TEST--
HttpServer syslog sink (#5, B5): datagram transports — one record per UDP/unix datagram
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip udg:// is POSIX-only');
?>
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
$udgPath  = sys_get_temp_dir() . '/php-http-025-' . getmypid() . '.sock';
@unlink($udgPath);

$udp = stream_socket_server("udp://127.0.0.1:$sysPort", $errno, $errstr, STREAM_SERVER_BIND);
$udg = stream_socket_server("udg://$udgPath", $errno2, $errstr2, STREAM_SERVER_BIND);
if (!$udp || !$udg) { echo "FAIL: bind $errstr $errstr2\n"; exit(1); }
stream_set_blocking($udp, false);
stream_set_blocking($udg, false);

/* Collect datagrams while the server runs; each recvfrom = one datagram, so
 * message boundaries survive iff the sink writes one record per datagram. */
$udpGrams = [];
$udgGrams = [];
$stop     = false;
spawn(function () use ($udp, $udg, &$udpGrams, &$udgGrams, &$stop) {
    while (!$stop) {
        while (($d = @stream_socket_recvfrom($udp, 65535)) !== false && $d !== '') {
            $udpGrams[] = $d;
        }
        while (($d = @stream_socket_recvfrom($udg, 65535)) !== false && $d !== '') {
            $udgGrams[] = $d;
        }
        usleep(10000);
    }
});

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $httpPort)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSinks([
        ['type' => 'syslog', 'target' => "udp://127.0.0.1:$sysPort",
         'facility' => 'local0', 'level' => LogSeverity::INFO],
        ['type' => 'syslog', 'target' => "udg://$udgPath",
         'facility' => 'daemon', 'level' => LogSeverity::INFO],
    ]);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });

spawn(function () use ($server) { usleep(200000); $server->stop(); });
$server->start();

usleep(150000);   /* let the collector drain the last datagrams */
$stop = true;
usleep(30000);    /* collector observes the flag before we close its sockets */
fclose($udp);
fclose($udg);
@unlink($udgPath);

/* Every datagram is exactly one bare RFC 5424 message: correct PRI, no
 * RFC 6587 length prefix. local0(16)*8+INFO(6)=134; daemon(3)*8+6=30. */
$check = function (array $grams, string $pri, string $tag) {
    $starts = 0;
    foreach ($grams as $g) {
        if (!str_starts_with($g, "<$pri>1 ")) {
            echo "$tag: bad frame: ", substr($g, 0, 32), "\n";
            return;
        }
        if (str_contains($g, 'server.start')) $starts++;
    }
    echo "$tag frames: ok\n";
    echo "$tag start: ", $starts === 1 ? "yes" : "no ($starts)", "\n";
};

$check($udpGrams, '134', 'udp');
$check($udgGrams, '30', 'udg');

echo "done\n";
?>
--EXPECT--
udp frames: ok
udp start: yes
udg frames: ok
udg start: yes
done
