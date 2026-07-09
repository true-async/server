--TEST--
HttpServer: duplicate headers combine per RFC 9110 §5.3 — ", " generic, "; " for cookie
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);
$server->addHttpHandler(function($req, $resp) {
    $h = $req->getHeaders();
    $resp->setStatusCode(200)
         ->setBody(($h['x-multi'] ?? '-') . '|' . ($h['cookie'] ?? '-'));
});

$client = spawn(function() use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    $req = "GET / HTTP/1.1\r\nHost: x\r\n"
         . "X-Multi: alpha\r\n"
         . "X-Multi: beta\r\n"
         . "Cookie: a=1\r\n"
         . "Cookie: b=2\r\n"
         . "Connection: close\r\n"
         . "\r\n";
    fwrite($fp, $req);
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) { $c = fread($fp, 8192); if ($c === '' || $c === false) break; $r .= $c; }
    fclose($fp);
    $body = substr($r, strpos($r, "\r\n\r\n") + 4);
    echo "body: $body\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
--EXPECT--
body: alpha, beta|a=1; b=2
done
