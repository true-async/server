--TEST--
HttpServerConfig: log severity / log stream / telemetry config (PLAN_LOG.md Step 1)
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;

// Backed enum — values match OTel SeverityNumber.
echo "OFF=",   LogSeverity::OFF->value,   "\n";
echo "DEBUG=", LogSeverity::DEBUG->value, "\n";
echo "INFO=",  LogSeverity::INFO->value,  "\n";
echo "WARN=",  LogSeverity::WARN->value,  "\n";
echo "ERROR=", LogSeverity::ERROR->value, "\n";

$c = new HttpServerConfig();

// Defaults: logger off, no stream, telemetry off.
echo "default sev=",    $c->getLogSeverity()->name, "\n";
echo "default stream=", var_export($c->getLogStream(), true), "\n";
echo "default tele=",   var_export($c->isTelemetryEnabled(), true), "\n";

// Setters return $this (fluent).
$ret = $c->setLogSeverity(LogSeverity::INFO);
var_dump($ret === $c);

$f = fopen("php://memory", "w+b");
$c->setLogStream($f);
echo "stream is resource=", var_export(is_resource($c->getLogStream()), true), "\n";

$c->setTelemetryEnabled(true);
echo "tele=", var_export($c->isTelemetryEnabled(), true), "\n";
echo "sev=", $c->getLogSeverity()->name, "(", $c->getLogSeverity()->value, ")\n";

// Type system rejects non-enum values automatically.
try {
    $c->setLogSeverity(99);
} catch (\TypeError $e) {
    echo "rejected non-enum int: ok\n";
}

// Invalid stream — non-resource.
try {
    $c->setLogStream("not a resource");
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "rejected non-resource: ok\n";
}

// Pass null — clears stream.
$c->setLogStream(null);
echo "after clear stream=", var_export($c->getLogStream(), true), "\n";

// Round-trip every case through the config.
foreach ([LogSeverity::OFF, LogSeverity::DEBUG, LogSeverity::INFO,
          LogSeverity::WARN, LogSeverity::ERROR] as $sev) {
    $c2 = new HttpServerConfig();
    $c2->setLogSeverity($sev);
    echo "roundtrip ", $sev->name, ": ",
         ($c2->getLogSeverity() === $sev ? "ok" : "FAIL"), "\n";
}

echo "Done\n";
--EXPECT--
OFF=0
DEBUG=5
INFO=9
WARN=13
ERROR=17
default sev=OFF
default stream=NULL
default tele=false
bool(true)
stream is resource=true
tele=true
sev=INFO(9)
rejected non-enum int: ok
rejected non-resource: ok
after clear stream=NULL
roundtrip OFF: ok
roundtrip DEBUG: ok
roundtrip INFO: ok
roundtrip WARN: ok
roundtrip ERROR: ok
Done
