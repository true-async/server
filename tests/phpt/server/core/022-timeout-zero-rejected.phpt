--TEST--
HttpServerConfig: setReadTimeout/setKeepAliveTimeout reject zero; setWriteTimeout accepts zero (= disabled)
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServerConfig;

$c = new HttpServerConfig();

foreach (['setReadTimeout', 'setKeepAliveTimeout'] as $m) {
    try {
        $c->$m(0);
        echo "$m(0): UNEXPECTED ACCEPT\n";
    } catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
        echo "$m(0): rejected\n";
    }
    try {
        $c->$m(-1);
        echo "$m(-1): UNEXPECTED ACCEPT\n";
    } catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
        echo "$m(-1): rejected\n";
    }
    // Positive still works
    $c->$m(30);
}

// setWriteTimeout(0) is now allowed — disables the per-conn write
// deadline timer. Negative is still rejected.
$c->setWriteTimeout(0);
echo "setWriteTimeout(0): accepted\n";
try {
    $c->setWriteTimeout(-1);
    echo "setWriteTimeout(-1): UNEXPECTED ACCEPT\n";
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "setWriteTimeout(-1): rejected\n";
}
$c->setWriteTimeout(30);

echo "positives accepted\n";

// setShutdownTimeout still accepts 0 (no grace period semantic is legitimate)
$c->setShutdownTimeout(0);
echo "setShutdownTimeout(0): accepted\n";

?>
--EXPECT--
setReadTimeout(0): rejected
setReadTimeout(-1): rejected
setKeepAliveTimeout(0): rejected
setKeepAliveTimeout(-1): rejected
setWriteTimeout(0): accepted
setWriteTimeout(-1): rejected
positives accepted
setShutdownTimeout(0): accepted
