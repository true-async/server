--TEST--
E2E: POST request with body
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$test->serverHandler(function($request, $response) {
    // Echo back method and body
    $method = $request->getMethod();
    $body = $request->getBody();

    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setBody("Method: $method\nBody: $body");
});

$body = '{"name":"test","value":123}';

$test->sendRequest(
    "POST /api/data HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Content-Type: application/json\r\n" .
    "Content-Length: " . strlen($body) . "\r\n" .
    "Connection: close\r\n" .
    "\r\n" .
    $body
);

$test->expectStatus(200);
$test->expectBody("Method: POST\nBody: $body");

$test->run();
--EXPECT--
OK
