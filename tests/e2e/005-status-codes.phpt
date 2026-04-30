--TEST--
E2E: Different HTTP status codes
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

// Test 404 Not Found
$test = new HttpTestCase();

$test->serverHandler(function($request, $response) {
    $response->setStatusCode(404);
    $response->setBody('Not Found');
});

$test->sendRequest(
    "GET /missing HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(404);
$test->expectBody('Not Found');

$test->run();
echo "404 test passed\n";

// Test 201 Created
$test2 = new HttpTestCase(port: 19900);

$test2->serverHandler(function($request, $response) {
    $response->setStatusCode(201);
    $response->setHeader('Location', '/items/123');
    $response->setBody('{"id":123}');
});

$test2->sendRequest(
    "POST /items HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test2->expectStatus(201);
$test2->expectHeader('location', '/items/123');

$test2->run();
echo "201 test passed\n";
--EXPECT--
OK
404 test passed
OK
201 test passed
