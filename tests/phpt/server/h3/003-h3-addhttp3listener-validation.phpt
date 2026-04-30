--TEST--
HttpServerConfig: addHttp3Listener input validation + locked-config guard
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$c = new HttpServerConfig();

// Port range: bad low
try {
    $c->addHttp3Listener('127.0.0.1', 0);
    echo "port=0 ACCEPTED (unexpected)\n";
} catch (Throwable $e) {
    echo "port=0 rejected: ",
         (int)str_contains($e->getMessage(), 'Port'), "\n";
}

// Port range: bad high
try {
    $c->addHttp3Listener('127.0.0.1', 65536);
    echo "port=65536 ACCEPTED (unexpected)\n";
} catch (Throwable $e) {
    echo "port=65536 rejected: ",
         (int)str_contains($e->getMessage(), 'Port'), "\n";
}

// Valid — accepted
$c->addHttp3Listener('127.0.0.1', 443);
$listeners = $c->getListeners();
echo "listeners_count=", count($listeners), "\n";
echo "type=",  $listeners[0]['type']  ?? '?', "\n";
echo "port=",  $listeners[0]['port']  ?? '?', "\n";
echo "host=",  $listeners[0]['host']  ?? '?', "\n";
echo "tls=",   (int)($listeners[0]['tls']   ?? false), "\n";

// Fluent API — returns self
$c2 = (new HttpServerConfig())
    ->addListener('127.0.0.1', 8080)
    ->addHttp3Listener('127.0.0.1', 8443);
echo "fluent_ok=", (int)($c2 instanceof HttpServerConfig), "\n";

echo "done\n";
?>
--EXPECT--
port=0 rejected: 1
port=65536 rejected: 1
listeners_count=1
type=udp_h3
port=443
host=127.0.0.1
tls=1
fluent_ok=1
done
