--TEST--
HttpServerConfig: Basic configuration
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServerConfig;

$config = new HttpServerConfig();

// Default listener
$config->addListener('127.0.0.1', 8080);
$listeners = $config->getListeners();
var_dump(count($listeners));
var_dump($listeners[0]['type']);
var_dump($listeners[0]['host']);
var_dump($listeners[0]['port']);

// Test fluent API
$config2 = (new HttpServerConfig())
    ->addListener('0.0.0.0', 9000)
    ->setReadTimeout(30)
    ->setWriteTimeout(60)
    ->setKeepAliveTimeout(120)
    ->setBacklog(256)
    ->setMaxConnections(1000);

echo "Backlog: " . $config2->getBacklog() . "\n";
echo "ReadTimeout: " . $config2->getReadTimeout() . "\n";
echo "WriteTimeout: " . $config2->getWriteTimeout() . "\n";
echo "KeepAliveTimeout: " . $config2->getKeepAliveTimeout() . "\n";
echo "MaxConnections: " . $config2->getMaxConnections() . "\n";

// Test locked config
echo "isLocked before HttpServer: " . ($config2->isLocked() ? 'true' : 'false') . "\n";

echo "Done\n";
--EXPECT--
int(1)
string(3) "tcp"
string(9) "127.0.0.1"
int(8080)
Backlog: 256
ReadTimeout: 30
WriteTimeout: 60
KeepAliveTimeout: 120
MaxConnections: 1000
isLocked before HttpServer: false
Done
