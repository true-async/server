--TEST--
E2E: Empty response body (204 No Content)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$test->serverHandler(function($request, $response) {
    $response->setStatusCode(204);
    // No body for 204
});

$test->sendRequest(
    "DELETE /item/123 HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(204);
$test->expectBody('');

$test->run();
--EXPECT--
OK
