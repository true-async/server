--TEST--
HttpServer: stop() closes keep-alive connections and starts no request behind the one in flight
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* After stop() a connection may finish the request it is running and nothing
 * more: start() cancels and empties the server scope, and a request started
 * after that runs in a scope php-async frees once it is empty. Two cases. An
 * idle keep-alive connection is closed, so a request sent once start() has
 * returned is not answered. A request in flight answers with Connection:
 * close, and the requests pipelined behind it are not run. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

function serve(int $port): HttpServer
{
    $server = new HttpServer((new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
        ->setShutdownTimeout(2));

    $server->addHttpHandler(function ($req, $res) {
        if ($req->getPath() === '/slow') {
            delay(300);
        }

        $res->setStatusCode(200)->setBody('ok')->end();
    });

    return $server;
}

function read_all($c): string
{
    stream_set_timeout($c, 2);
    $in = '';

    while (!feof($c)) {
        $chunk = fread($c, 65536);
        if ($chunk === false || $chunk === '') {
            break;
        }

        $in .= $chunk;
    }

    return $in;
}

/* Idle: one request answered before stop(), one sent after start() returned. */
$port = tas_free_port();
$server = serve($port);
$c = null;
$client = spawn(function () use ($port, $server, &$c) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($c, "GET /a HTTP/1.1\r\nHost: t\r\n\r\n");
    $head = fread($c, 4096);
    echo "idle: before stop ", str_starts_with($head, "HTTP/1.1 200") ? 'answered' : 'missing', "\n";
    $server->stop();
});
$server->start();
await($client);

@fwrite($c, "GET /b HTTP/1.1\r\nHost: t\r\n\r\n");
delay(100);
$in = read_all($c);
echo "idle: after stop ", substr_count($in, "HTTP/1.1 200"), " answered, ",
    feof($c) ? 'closed' : 'open', "\n";
fclose($c);

/* In flight: /slow is running when stop() comes, three requests behind it. */
$port = tas_free_port();
$server = serve($port);
$client = spawn(function () use ($port, $server) {
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    fwrite($c, "GET /slow HTTP/1.1\r\nHost: t\r\n\r\n"
        . str_repeat("GET /next HTTP/1.1\r\nHost: t\r\n\r\n", 3));
    delay(100);
    $server->stop();
    $in = read_all($c);
    echo "in flight: ", substr_count($in, "HTTP/1.1 200"), " answered, ",
        stripos($in, "Connection: close") !== false ? 'Connection: close' : 'no close', ", ",
        feof($c) ? 'closed' : 'open', "\n";
    fclose($c);
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
idle: before stop answered
idle: after stop 0 answered, closed
in flight: 1 answered, Connection: close, closed
done
