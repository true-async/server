--TEST--
HttpServer: a query string past max_input_vars is answered 400 when the handler lets the refusal go (#413)
--EXTENSIONS--
true_async_server
true_async
--INI--
max_input_vars=3
--FILE--
<?php
/* getQuery() kept the first max_input_vars pairs and dropped the rest without
 * a word, so a handler served a filter or a list that was silently cut. It
 * throws HttpException 400 now, as a form body past the limit does, and
 * uncaught that answers the request. A query within the limit is read. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody(json_encode($req->getQuery()));
});

$get = static function (int $port, string $target): string {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    fwrite($fp, "GET $target HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($fp);
    fclose($fp);

    $status = substr($reply, 9, 3);
    $body   = substr($reply, strpos($reply, "\r\n\r\n") + 4);

    return "$status $body";
};

$client = spawn(function () use ($port, $server, $get) {
    usleep(30000);
    echo 'within: ', $get($port, '/?a=1&b=2&c=3'), "\n";
    echo 'past:   ', $get($port, '/?a=1&b=2&c=3&d=4'), "\n";
    $server->stop();
});

$server->start();
await($client);
?>
--EXPECT--
within: 200 {"a":"1","b":"2","c":"3"}
past:   400 the query string was refused: more fields than max_input_vars
