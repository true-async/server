--TEST--
HttpServer: start() from a handler of the run that stop() is ending is refused, and leaves the server able to start again (#376)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A handler runs inside the server scope of its run, and start() drains that
 * scope after stop() wakes it. A start() the handler calls after stop() would
 * begin a run inside the one still ending: it is refused before it touches
 * the server, and the start() after the outer one returns serves as usual. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $res) use ($server) {
    if ($req->getPath() === '/restart') {
        /* Refused as a running server's, and must leave the run marked. */
        try {
            $server->start();
        } catch (Throwable $e) {
            echo "start() while running: ", $e->getMessage(), "\n";
        }

        $server->stop();

        try {
            $server->start();
            echo "nested start(): returned\n";
        } catch (Throwable $e) {
            echo "nested start(): ", get_class($e), ": ", $e->getMessage(), "\n";
        }

        $res->setBody('restart');
        return;
    }

    $res->setBody('ok');
});

function get(int $port, string $path): string
{
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if ($c === false) {
        return "connect failed: $errstr";
    }

    fwrite($c, "GET $path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    $reply = stream_get_contents($c);
    fclose($c);

    return strtok((string) $reply, "\r\n") . ' / ' . substr((string) $reply, strrpos((string) $reply, "\n") + 1);
}

$client = spawn(function () use ($port) {
    delay(100);
    return get($port, '/restart');
});
$started = $server->start();
echo "/restart: ", await($client), "\n";
var_dump($started);

$client = spawn(function () use ($port, $server) {
    delay(100);
    $reply = get($port, '/');
    $server->stop();
    return $reply;
});
$started = $server->start();
echo "/: ", await($client), "\n";
var_dump($started);
?>
--EXPECT--
start() while running: Server is already running
nested start(): TrueAsync\HttpServerRuntimeException: Server is still stopping: start() was called before the previous start() returned
/restart: HTTP/1.1 200 OK / restart
bool(true)
/: HTTP/1.1 200 OK / ok
bool(true)
