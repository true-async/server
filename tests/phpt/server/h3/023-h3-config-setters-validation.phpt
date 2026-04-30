--TEST--
HttpServerConfig: H3 production setters input validation + locked-config guard (NEXT_STEPS.md §5)
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

// === defaults ===
echo "default idle=",     $c->getHttp3IdleTimeoutMs(), "\n";
echo "default window=",   $c->getHttp3StreamWindowBytes(), "\n";
echo "default streams=",  $c->getHttp3MaxConcurrentStreams(), "\n";
echo "default peer=",     $c->getHttp3PeerConnectionBudget(), "\n";

// === valid sets are persisted ===
$c->setHttp3IdleTimeoutMs(120000)
  ->setHttp3StreamWindowBytes(4 * 1024 * 1024)
  ->setHttp3MaxConcurrentStreams(500)
  ->setHttp3PeerConnectionBudget(64);
echo "set idle=",     $c->getHttp3IdleTimeoutMs(), "\n";
echo "set window=",   $c->getHttp3StreamWindowBytes(), "\n";
echo "set streams=",  $c->getHttp3MaxConcurrentStreams(), "\n";
echo "set peer=",     $c->getHttp3PeerConnectionBudget(), "\n";

// === negative / out-of-range rejection ===
function expect_reject(callable $fn, string $label): void {
    try { $fn(); echo "$label ACCEPTED\n"; }
    catch (Throwable $e) { echo "$label rejected\n"; }
}
expect_reject(fn() => $c->setHttp3IdleTimeoutMs(-1),                   "idle<0");
expect_reject(fn() => $c->setHttp3StreamWindowBytes(0),                "window=0");
expect_reject(fn() => $c->setHttp3StreamWindowBytes(1023),             "window<1024");
expect_reject(fn() => $c->setHttp3StreamWindowBytes(2 * 1024 * 1024 * 1024), "window>1GiB");
expect_reject(fn() => $c->setHttp3MaxConcurrentStreams(0),             "streams=0");
expect_reject(fn() => $c->setHttp3MaxConcurrentStreams(-5),            "streams<0");
expect_reject(fn() => $c->setHttp3MaxConcurrentStreams(2_000_000),     "streams>1M");
expect_reject(fn() => $c->setHttp3PeerConnectionBudget(0),             "peer=0");
expect_reject(fn() => $c->setHttp3PeerConnectionBudget(5000),          "peer>4096");

// === idle accepts 0 (means "advertise no idle timeout") ===
$c->setHttp3IdleTimeoutMs(0);
echo "idle=0 accepted=", $c->getHttp3IdleTimeoutMs() === 0 ? 1 : 0, "\n";

// === locked-config guard. Need a server start to lock; do a quick
//     synthetic lock via constructing the server (constructor freezes
//     the config, marks is_locked = true). ===
$c2 = new HttpServerConfig();
$c2->addListener('127.0.0.1', 19999);
$srv = new HttpServer($c2);
expect_reject(fn() => $c2->setHttp3IdleTimeoutMs(60000),          "idle locked");
expect_reject(fn() => $c2->setHttp3StreamWindowBytes(1024 * 1024),"window locked");
expect_reject(fn() => $c2->setHttp3MaxConcurrentStreams(50),      "streams locked");
expect_reject(fn() => $c2->setHttp3PeerConnectionBudget(8),       "peer locked");

echo "ok\n";
?>
--EXPECT--
default idle=30000
default window=262144
default streams=100
default peer=16
set idle=120000
set window=4194304
set streams=500
set peer=64
idle<0 rejected
window=0 rejected
window<1024 rejected
window>1GiB rejected
streams=0 rejected
streams<0 rejected
streams>1M rejected
peer=0 rejected
peer>4096 rejected
idle=0 accepted=1
idle locked rejected
window locked rejected
streams locked rejected
peer locked rejected
ok
