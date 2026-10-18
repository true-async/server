--TEST--
HttpServer: higher HTTP/1 minor uses HTTP/1.1 responses and preserves pipelining
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
$server = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(3)->setWriteTimeout(3));
$handled = 0;
$server->addHttpHandler(function ($req, $res) use (&$handled) {
    $res->setBody('response-' . ++$handled)->end();
});
$client = spawn(function () use ($port, $server, &$handled) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    stream_set_timeout($fp, 3);
    fwrite($fp, "GET /first HTTP/1.2\r\nHost: x\r\n\r\n"
        . "GET /second HTTP/1.9\r\nHost: x\r\nConnection: close\r\n\r\n");
    $wire = stream_get_contents($fp);
    fclose($fp);
    echo 'responses=', substr_count($wire, 'HTTP/1.1 200 OK'), "\n";
    echo 'handled=', $handled, "\n";
    $a = strpos($wire, 'response-1');
    $b = strpos($wire, 'response-2');
    echo 'ordered=', ($a !== false && $b !== false && $a < $b ? 'yes' : 'no'), "\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
responses=2
handled=2
ordered=yes
done
