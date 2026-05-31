--TEST--
HttpServerConfig: setHttp3SocketBufferBytes — default, range validation, locked-config guard (#59)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif([]);
?>
--FILE--
<?php
/* setHttp3SocketBufferBytes had zero coverage anywhere in the suite.
 * It is the SO_*BUF sizing knob: under-sizing silently drops inbound
 * bursts (RcvbufErrors -> peer PTO). Pure config-surface — no traffic.
 * Verifies default (8 MiB), valid set + readback, range rejection
 * (negative, >256 MiB), the 0 = "leave OS default" accept, and the
 * locked-config guard once the config is handed to a server. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$c = new HttpServerConfig();

// === default ===
echo "default=", $c->getHttp3SocketBufferBytes(), "\n";   /* 8 MiB */

// === valid set is persisted, setter is chainable ===
$ret = $c->setHttp3SocketBufferBytes(16 * 1024 * 1024);
echo "chain=", ($ret === $c ? 1 : 0), "\n";
echo "set=", $c->getHttp3SocketBufferBytes(), "\n";

// === 0 means "leave the OS default untouched" — accepted ===
$c->setHttp3SocketBufferBytes(0);
echo "zero=", $c->getHttp3SocketBufferBytes() === 0 ? 1 : 0, "\n";

// === max boundary (256 MiB) accepted; one past it rejected ===
$c->setHttp3SocketBufferBytes(256 * 1024 * 1024);
echo "max=", $c->getHttp3SocketBufferBytes(), "\n";

function expect_reject(callable $fn, string $label): void {
    try { $fn(); echo "$label ACCEPTED\n"; }
    catch (Throwable $e) { echo "$label rejected\n"; }
}
expect_reject(fn() => $c->setHttp3SocketBufferBytes(-1),                  "neg");
expect_reject(fn() => $c->setHttp3SocketBufferBytes(256 * 1024 * 1024 + 1), "over256MiB");

// === locked-config guard — constructing the server freezes the config ===
$c2 = new HttpServerConfig();
$c2->addListener('127.0.0.1', 19998);
$srv = new HttpServer($c2);
expect_reject(fn() => $c2->setHttp3SocketBufferBytes(4 * 1024 * 1024), "locked");

echo "ok\n";
?>
--EXPECT--
default=8388608
chain=1
set=16777216
zero=1
max=268435456
neg rejected
over256MiB rejected
locked rejected
ok
