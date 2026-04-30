--TEST--
HttpServer: Basic instantiation
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', 18080)  // Test port
    ->setReadTimeout(30)
    ->setWriteTimeout(30);

$server = new HttpServer($config);

var_dump($server instanceof HttpServer);
var_dump($server->isRunning());

// Config should be locked after server creation
var_dump($config->isLocked());

// getConfig returns same config
$serverConfig = $server->getConfig();
var_dump($serverConfig === $config);

// getTelemetry
$telemetry = $server->getTelemetry();
var_dump(is_array($telemetry));
var_dump(isset($telemetry['total_requests']));
var_dump(isset($telemetry['active_connections']));

echo "Done\n";
--EXPECT--
bool(true)
bool(false)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
Done
