--TEST--
E2E: Query parameters in URI
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$test->serverHandler(function($request, $response) {
    $uri = $request->getUri();

    $response->setStatusCode(200);
    $response->setBody("URI: $uri");
});

$test->sendRequest(
    "GET /search?q=hello+world&page=1&limit=10 HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

$test->expectStatus(200);
$test->expectBody("URI: /search?q=hello+world&page=1&limit=10");

$test->run();
--EXPECT--
OK
