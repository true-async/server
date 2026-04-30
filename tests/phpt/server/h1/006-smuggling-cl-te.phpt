--TEST--
HttpServer: reject simultaneous Content-Length + Transfer-Encoding (RFC 9112 §6.3, S-01)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19822 + getmypid() % 1000;
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);
$server->addHttpHandler(function($r,$s){ $s->setStatusCode(200)->end(); });

$client = spawn(function() use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    // Smuggling vector: both Content-Length and Transfer-Encoding: chunked
    $req = "POST / HTTP/1.1\r\nHost: x\r\n"
         . "Content-Length: 5\r\n"
         . "Transfer-Encoding: chunked\r\n"
         . "\r\n0\r\n\r\n";
    fwrite($fp, $req);
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) { $c = fread($fp, 8192); if ($c === '' || $c === false) break; $r .= $c; }
    fclose($fp);
    $lines = explode("\r\n", $r);
    echo "status: " . $lines[0] . "\n";
    $tel = $server->getTelemetry();
    echo "parse_errors_400_total=" . $tel['parse_errors_400_total'] . "\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECTF--
status: HTTP/1.1 400 Bad Request
parse_errors_400_total=1
done
