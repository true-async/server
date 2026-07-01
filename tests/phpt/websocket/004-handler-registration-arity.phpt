--TEST--
WebSocket: addWebSocketHandler accepts both 2-arg and 3-arg handler signatures
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$config = new HttpServerConfig();
$config->addListener('127.0.0.1', 12345);   // never actually bound — server is not started

$server = new HttpServer($config);

// 2-arg handler: server accepts the upgrade with defaults.
$server->addWebSocketHandler(function ($ws, $req) {});
echo "2-arg: ok\n";

// 3-arg handler: server invokes with WebSocketUpgrade as the third argument.
$server->addWebSocketHandler(function ($ws, $req, $upgrade) {});
echo "3-arg: ok\n";

echo "Done\n";
--EXPECT--
2-arg: ok
3-arg: ok
Done
