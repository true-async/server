--TEST--
HttpServer: End-to-end simple GET request
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19800 + getmypid() % 1000;  // Random-ish port to avoid conflicts

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$requestReceived = false;
$requestMethod = null;
$requestUri = null;

$server->addHttpHandler(function($request, $response) use (&$requestReceived, &$requestMethod, &$requestUri, $server) {
    $requestReceived = true;
    $requestMethod = $request->getMethod();
    $requestUri = $request->getUri();

    $response->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('X-Test', 'hello')
        ->setBody('Hello from server!');

    $response->end();

    // Stop server after first request
    $server->stop();
});

// Spawn client coroutine — hold handle so we can await it before printing
// server-side results (otherwise the main coroutine races with the client).
$clientCoroutine = spawn(function() use ($port) {
    // Small delay to let server start
    usleep(10000);

    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        echo "Client connect failed: $errstr\n";
        return;
    }

    // Send HTTP request
    $request = "GET /test/path?foo=bar HTTP/1.1\r\n";
    $request .= "Host: localhost\r\n";
    $request .= "Connection: close\r\n";
    $request .= "\r\n";

    fwrite($fp, $request);

    // Read response
    $response = '';
    while (!feof($fp)) {
        $response .= fread($fp, 8192);
    }
    fclose($fp);

    echo "=== Client received ===\n";
    echo $response;
});

// Start server (will run until stop() is called)
$server->start();

// Wait for the client to finish reading before we print server-side info.
await($clientCoroutine);

echo "\n=== Server side ===\n";
echo "Request received: " . ($requestReceived ? 'yes' : 'no') . "\n";
echo "Method: $requestMethod\n";
echo "URI: $requestUri\n";
--EXPECTF--
=== Client received ===
HTTP/1.1 200 OK
Content-Length: 18
content-type: text/plain
x-test: hello

Hello from server!
=== Server side ===
Request received: yes
Method: GET
URI: /test/path?foo=bar
