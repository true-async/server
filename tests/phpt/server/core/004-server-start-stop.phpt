--TEST--
HttpServer: Start and stop without requests
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

$port = 19900 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$server->addHttpHandler(function($request, $response) {
    $response->setStatusCode(200)->setBody('OK')->end();
});

echo "isRunning before start: " . ($server->isRunning() ? 'yes' : 'no') . "\n";

// Spawn a coroutine that stops the server after a delay
spawn(function() use ($server) {
    usleep(50000);  // 50ms
    echo "Stopping server...\n";
    $server->stop();
});

echo "Starting server...\n";
$server->start();

echo "Server stopped\n";
echo "isRunning after stop: " . ($server->isRunning() ? 'yes' : 'no') . "\n";

echo "Done\n";
--EXPECT--
isRunning before start: no
Starting server...
Stopping server...
Server stopped
isRunning after stop: no
Done
