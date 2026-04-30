--TEST--
HttpServer: handler throws HttpException — server uses its code+message
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;

$port = 19820 + getmypid() % 1000;
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    // Public API: handler throws HttpException, dispose builds the
    // response from its code (HTTP status) and message (body).
    throw new HttpException("Resource not found here", 404);
});

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    fwrite($fp, "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n");
    stream_set_timeout($fp, 2);
    $r = '';
    while (!feof($fp)) { $c = fread($fp, 8192); if ($c === '' || $c === false) break; $r .= $c; }
    fclose($fp);
    $lines = explode("\r\n", $r);
    echo "status: " . $lines[0] . "\n";
    // Body should contain our message
    echo "has-body: " . (strpos($r, 'Resource not found here') !== false ? 'yes' : 'no') . "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
status: HTTP/1.1 404%a
has-body: yes
done
