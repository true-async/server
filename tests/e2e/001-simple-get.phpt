--TEST--
E2E: Simple GET request
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$test->serverHandler(function($request, $response) {
    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setBody('Hello World!');
});

// Raw HTTP request
$test->sendRequest(
    "GET /test HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(200);
$test->expectHeader('content-type', 'text/plain');
$test->expectBody('Hello World!');

$test->run();
--EXPECT--
OK
