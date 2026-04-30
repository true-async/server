--TEST--
E2E: Large response body
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

// Generate 100KB of data
$largeBody = str_repeat('ABCDEFGHIJ', 10240);

$test->serverHandler(function($request, $response) use ($largeBody) {
    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'application/octet-stream');
    $response->setBody($largeBody);
});

$test->sendRequest(
    "GET /large HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(200);
$test->expectHeader('content-type', 'application/octet-stream');

// Custom validator to check size
$test->validate(function($status, $headers, $body) use ($largeBody) {
    if (strlen($body) !== strlen($largeBody)) {
        return "Body size mismatch: expected " . strlen($largeBody) . ", got " . strlen($body);
    }
    if ($body !== $largeBody) {
        return "Body content mismatch";
    }
    return true;
});

$test->run();
echo "Large body size: " . strlen($largeBody) . " bytes\n";
--EXPECT--
OK
Large body size: 102400 bytes
