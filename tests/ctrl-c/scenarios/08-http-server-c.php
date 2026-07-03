<?php
# MARKER: SIGNAL-WOKE
/* Our C HttpServer with an Async\signal-driven stop(): does SIGINT
 * reach userland while the C server owns the reactor? */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

if (!class_exists(HttpServer::class)) {
    echo "SKIP true_async_server not loaded\n";
    exit(0);
}

$port = (int) (getenv('CTRLC_PORT') ?: 18938);

$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    $res->setBody("ok")->end();
});

spawn(function () use ($server) {
    \Async\await_any_or_fail([
        \Async\signal(\Async\Signal::SIGINT),
        \Async\signal(\Async\Signal::SIGTERM),
    ]);
    echo "SIGNAL-WOKE\n";
    $server->stop();
});

spawn(function () use ($port) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:{$port}", $e, $s, 0.05);
        if ($fp) {
            fclose($fp);
            echo "READY\n";
            return;
        }
        \Async\delay(20);
    }
    echo "BORK listener never came up\n";
});

$server->start();
echo "SHUTDOWN-COMPLETE\n";
