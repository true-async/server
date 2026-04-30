--TEST--
HttpServerConfig: setCertificate/setPrivateKey validate path at setter time
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServerConfig;

$c = new HttpServerConfig();

// Non-existent path rejected
try {
    $c->setCertificate('/nonexistent/cert.pem');
    echo "cert nonexistent: UNEXPECTED ACCEPT\n";
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "cert nonexistent: rejected\n";
}

try {
    $c->setPrivateKey('/nonexistent/key.pem');
    echo "key nonexistent: UNEXPECTED ACCEPT\n";
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "key nonexistent: rejected\n";
}

// Directory (not a regular file) rejected
try {
    $c->setCertificate('/tmp');
    echo "cert dir: UNEXPECTED ACCEPT\n";
} catch (\TrueAsync\HttpServerInvalidArgumentException $e) {
    echo "cert dir: rejected\n";
}

// Existing readable file accepted. Use PHP's own binary as a stand-in
// for "any regular readable file" — always present in test env.
$realFile = PHP_BINARY;
$c->setCertificate($realFile);
echo "cert real file: accepted\n";
echo $c->getCertificate() === $realFile ? "getter roundtrip: ok\n" : "getter roundtrip: MISMATCH\n";
?>
--EXPECT--
cert nonexistent: rejected
key nonexistent: rejected
cert dir: rejected
cert real file: accepted
getter roundtrip: ok
