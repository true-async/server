--TEST--
HttpServerConfig: Brotli + zstd level setters, defaults, locked guards (#9)
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

// Defaults — match HTTP_COMPRESSION_BROTLI_DEFAULT_LEVEL / _ZSTD_DEFAULT_LEVEL.
echo "br default=",   $c->getBrotliLevel(), "\n";
echo "zstd default=", $c->getZstdLevel(), "\n";

// Valid sets persist + chain.
$c->setBrotliLevel(0)->setZstdLevel(1);
echo "br set=",   $c->getBrotliLevel(), "\n";
echo "zstd set=", $c->getZstdLevel(), "\n";

$c->setBrotliLevel(11)->setZstdLevel(22);
echo "br max=",   $c->getBrotliLevel(), "\n";
echo "zstd max=", $c->getZstdLevel(), "\n";

function expect_reject(callable $fn, string $label): void {
    try { $fn(); echo "$label ACCEPTED\n"; }
    catch (Throwable $e) { echo "$label rejected\n"; }
}

// Out-of-range rejection.
expect_reject(fn() => $c->setBrotliLevel(-1), "br<0");
expect_reject(fn() => $c->setBrotliLevel(12), "br>11");
expect_reject(fn() => $c->setZstdLevel(0),    "zstd=0");
expect_reject(fn() => $c->setZstdLevel(23),   "zstd>22");

// Locked-config guard via HttpServer ctor.
$c2 = (new HttpServerConfig())->addListener('127.0.0.1', 19997);
$srv = new HttpServer($c2);
expect_reject(fn() => $c2->setBrotliLevel(5), "br locked");
expect_reject(fn() => $c2->setZstdLevel(5),   "zstd locked");

echo "Done\n";
?>
--EXPECT--
br default=4
zstd default=3
br set=0
zstd set=1
br max=11
zstd max=22
br<0 rejected
br>11 rejected
zstd=0 rejected
zstd>22 rejected
br locked rejected
zstd locked rejected
Done
