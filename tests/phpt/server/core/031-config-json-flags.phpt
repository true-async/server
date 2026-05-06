--TEST--
HttpServerConfig::setJsonEncodeFlags — defaults, validation, locked guard
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

/* Default = JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES (192). */
$expected = JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES;
echo "default=", ($c->getJsonEncodeFlags() === $expected) ? 'ok' : 'WRONG', "\n";

/* Setter accepts arbitrary 32-bit bitmask. */
$c->setJsonEncodeFlags(JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE);
echo "set=", $c->getJsonEncodeFlags() === (JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE) ? 'ok' : 'WRONG', "\n";

/* Negative is rejected. */
try { $c->setJsonEncodeFlags(-1); echo "neg ACCEPTED\n"; }
catch (Throwable $e) { echo "neg rejected\n"; }

/* Locked-config guard via HttpServer ctor. */
$c2 = (new HttpServerConfig())->addListener('127.0.0.1', 19996);
$srv = new HttpServer($c2);
try { $c2->setJsonEncodeFlags(0); echo "locked ACCEPTED\n"; }
catch (Throwable $e) { echo "locked rejected\n"; }

echo "Done\n";
?>
--EXPECT--
default=ok
set=ok
neg rejected
locked rejected
Done
