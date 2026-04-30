--TEST--
E2E: Request headers access
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$test->serverHandler(function($request, $response) {
    // Read custom headers
    $custom = $request->getHeader('X-Custom-Header');
    $auth = $request->getHeader('Authorization');

    $response->setStatusCode(200);
    $response->setBody("Custom: $custom\nAuth: $auth");
});

$test->sendRequest(
    "GET /test HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "X-Custom-Header: my-value-123\r\n" .
    "Authorization: Bearer token_xyz\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(200);
$test->expectBody("Custom: my-value-123\nAuth: Bearer token_xyz");

$test->run();
--EXPECT--
OK
