--TEST--
E2E: Various HTTP methods
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$methods = ['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'OPTIONS', 'HEAD'];
$port = 19800;

foreach ($methods as $method) {
    $test = new HttpTestCase(port: $port++);

    $test->serverHandler(function($request, $response) {
        $method = $request->getMethod();
        $response->setStatusCode(200);
        $response->setBody("Method: $method");
    });

    $test->sendRequest(
        "$method /resource HTTP/1.1\r\n" .
        "Host: localhost\r\n" .
        "Connection: close\r\n" .
        "\r\n"
    );

    $test->expectStatus(200);

    // HEAD should have no body
    if ($method !== 'HEAD') {
        $test->expectBody("Method: $method");
    }

    $test->run();
    echo "$method: passed\n";
}
--EXPECT--
OK
GET: passed
OK
POST: passed
OK
PUT: passed
OK
DELETE: passed
OK
PATCH: passed
OK
OPTIONS: passed
OK
HEAD: passed
