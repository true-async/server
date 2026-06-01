--TEST--
HttpServer: graceful shutdown drains in-flight per-request coroutines (issue #74)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
// A handler suspended in a per-request child scope must not be left in flight
// when the server scope is disposed at shutdown. With setShutdownTimeout(0)
// the still-parked /park handler is force-cancelled; a /work handler that
// finishes inside the grace window completes normally. Without the drain the
// process would hang on the parked coroutine (or abort in a debug build).
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 20180 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setShutdownTimeout(0);          // no grace: force-cancel whatever is parked

$server = new HttpServer($config);

$server->addHttpHandler(function ($request, $response) use ($server) {
    $uri = $request->getUri();
    if (str_starts_with($uri, '/park')) {
        Async\request_context()->set('rid', ltrim($uri, '/'));
        Async\delay(60000);           // stays parked across shutdown
        $response->setStatusCode(200)->setBody("late\n")->end();
        return;
    }
    $response->setStatusCode(200)->setBody("stopping\n")->end();
    $server->stop();
});

$client = spawn(function () use ($port) {
    usleep(40000);
    // Fire the parked request first (do not wait for its response).
    $park = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
    if ($park) {
        fwrite($park, "GET /park HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    }
    usleep(40000);
    // Now stop the server while /park is still suspended.
    $stop = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
    if ($stop) {
        fwrite($stop, "GET /stop HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        $buf = '';
        while (!feof($stop)) { $buf .= fread($stop, 8192); }
        fclose($stop);
        echo str_contains($buf, "stopping") ? "stop responded\n" : "stop missing\n";
    }
    if ($park) { @fclose($park); }
});

$server->start();
await($client);
echo "server stopped cleanly\n";
?>
--EXPECT--
stop responded
server stopped cleanly
