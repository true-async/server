--TEST--
E2E: Content-Length header is set correctly
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$testBody = 'Hello, this is exactly 35 bytes!!!';

$test->serverHandler(function($request, $response) use ($testBody) {
    $response->setStatusCode(200);
    $response->setBody($testBody);
});

$test->sendRequest(
    "GET / HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(200);
$test->expectHeader('content-length', (string)strlen($testBody));
$test->expectBody($testBody);

$test->run();
--EXPECT--
OK
