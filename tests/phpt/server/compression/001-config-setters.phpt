--TEST--
HttpServerConfig: compression setter validation, defaults, locked-config guard (#8)
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

// === defaults — match HTTP_COMPRESSION_DEFAULT_* in include/compression/http_compression_defaults.h ===
echo "enabled=",  (int)$c->isCompressionEnabled(), "\n";
echo "level=",    $c->getCompressionLevel(), "\n";
echo "minSize=",  $c->getCompressionMinSize(), "\n";
echo "reqMax=",   $c->getRequestMaxDecompressedSize(), "\n";

// Default whitelist materialised so getter shows the live policy.
$mt = $c->getCompressionMimeTypes();
sort($mt);
echo "mime[default]=", implode(",", $mt), "\n";

// === valid sets persist + chain ===
$c->setCompressionEnabled(false)
  ->setCompressionLevel(9)
  ->setCompressionMinSize(2048)
  ->setRequestMaxDecompressedSize(4 * 1024 * 1024);
echo "set enabled=", (int)$c->isCompressionEnabled(), "\n";
echo "set level=",   $c->getCompressionLevel(), "\n";
echo "set min=",     $c->getCompressionMinSize(), "\n";
echo "set reqMax=",  $c->getRequestMaxDecompressedSize(), "\n";

// === reqMax=0 is allowed (must be explicit "no cap") ===
$c->setRequestMaxDecompressedSize(0);
echo "reqMax=0 accepted=", $c->getRequestMaxDecompressedSize() === 0 ? 1 : 0, "\n";

// === MIME setter REPLACES wholesale, normalises (lowercase, strip params, trim) ===
$c->setCompressionMimeTypes([
    'TEXT/HTML',                              // case-fold
    '  application/json ; charset=utf-8 ',    // trim + strip param
    'application/json',                       // dedup vs the above after normalisation
]);
$mt = $c->getCompressionMimeTypes();
sort($mt);
echo "mime[set]=", implode(",", $mt), "\n";

// === negative / out-of-range rejection ===
function expect_reject(callable $fn, string $label): void {
    try { $fn(); echo "$label ACCEPTED\n"; }
    catch (Throwable $e) { echo "$label rejected\n"; }
}
expect_reject(fn() => $c->setCompressionLevel(0),                 "level=0");
expect_reject(fn() => $c->setCompressionLevel(10),                "level=10");
expect_reject(fn() => $c->setCompressionLevel(-1),                "level<0");
expect_reject(fn() => $c->setCompressionMinSize(-1),              "min<0");
expect_reject(fn() => $c->setCompressionMinSize(64*1024*1024),    "min>16MiB");
expect_reject(fn() => $c->setRequestMaxDecompressedSize(-1),      "reqMax<0");
expect_reject(fn() => $c->setCompressionMimeTypes([123]),         "mime not string");
expect_reject(fn() => $c->setCompressionMimeTypes(['  ;x']),      "mime empty after strip");

// === locked-config guard via HttpServer ctor ===
$c2 = new HttpServerConfig();
$c2->addListener('127.0.0.1', 19998);
$srv = new HttpServer($c2);
expect_reject(fn() => $c2->setCompressionEnabled(false),          "enabled locked");
expect_reject(fn() => $c2->setCompressionLevel(3),                "level locked");
expect_reject(fn() => $c2->setCompressionMinSize(8192),           "min locked");
expect_reject(fn() => $c2->setCompressionMimeTypes(['text/html']),"mime locked");
expect_reject(fn() => $c2->setRequestMaxDecompressedSize(1024),   "reqMax locked");

echo "Done\n";
?>
--EXPECT--
enabled=1
level=6
minSize=1024
reqMax=10485760
mime[default]=application/javascript,application/json,application/xml,image/svg+xml,text/css,text/html,text/javascript,text/plain,text/xml
set enabled=0
set level=9
set min=2048
set reqMax=4194304
reqMax=0 accepted=1
mime[set]=application/json,text/html
level=0 rejected
level=10 rejected
level<0 rejected
min<0 rejected
min>16MiB rejected
reqMax<0 rejected
mime not string rejected
mime empty after strip rejected
enabled locked rejected
level locked rejected
min locked rejected
mime locked rejected
reqMax locked rejected
Done
