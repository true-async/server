--TEST--
HttpServerConfig: backpressure target setter/getter + validation
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServerConfig;

$c = new HttpServerConfig();

// Default value is RFC 8289 CoDel target
var_dump($c->getBackpressureTargetMs());

// Setting and reading back
$c->setBackpressureTargetMs(20);
var_dump($c->getBackpressureTargetMs());

// Zero disables CoDel (hard cap still works)
$c->setBackpressureTargetMs(0);
var_dump($c->getBackpressureTargetMs());

// Fluent API — same return value as other setters
$c2 = (new HttpServerConfig())
    ->setMaxConnections(500)
    ->setBackpressureTargetMs(10);
var_dump($c2->getBackpressureTargetMs());
var_dump($c2->getMaxConnections());

// Negative is rejected
try {
    $c->setBackpressureTargetMs(-1);
    echo "FAIL: negative should throw\n";
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "rejected negative: ok\n";
}

// Absurdly large is rejected (10s cap)
try {
    $c->setBackpressureTargetMs(100000);
    echo "FAIL: 100000 should throw\n";
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "rejected too-large: ok\n";
}

// Edge: exactly 10000 is allowed
$c->setBackpressureTargetMs(10000);
var_dump($c->getBackpressureTargetMs());

echo "Done\n";
--EXPECT--
int(5)
int(20)
int(0)
int(10)
int(500)
rejected negative: ok
rejected too-large: ok
int(10000)
Done
