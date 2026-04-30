--TEST--
HttpServer: reject Content-Length with sign/non-digit (RFC 9110 §8.6, S-03)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19824 + getmypid() % 1000;
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);
$server->addHttpHandler(function($r,$s){ $s->setStatusCode(200)->end(); });

$client = spawn(function() use ($port, $server) {
    usleep(20000);
    // Negative CL — bare strtoul() would silently turn this into UINT64_MAX
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    $req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n";
    fwrite($fp, $req);
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) { $c = fread($fp, 8192); if ($c === '' || $c === false) break; $r .= $c; }
    fclose($fp);
    $lines = explode("\r\n", $r);
    echo "neg-status: " . $lines[0] . "\n";

    // Trailing junk — "100abc" must not parse as 100
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    $req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100abc\r\n\r\n";
    fwrite($fp, $req);
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) { $c = fread($fp, 8192); if ($c === '' || $c === false) break; $r .= $c; }
    fclose($fp);
    $lines = explode("\r\n", $r);
    echo "junk-status: " . $lines[0] . "\n";

    $tel = $server->getTelemetry();
    echo "parse_errors_400_total=" . $tel['parse_errors_400_total'] . "\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECTF--
neg-status: HTTP/1.1 400 Bad Request
junk-status: HTTP/1.1 400 Bad Request
parse_errors_400_total=2
done
