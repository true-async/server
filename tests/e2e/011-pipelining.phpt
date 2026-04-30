--TEST--
E2E: HTTP/1.1 keep-alive pipelining (two requests in one write)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();

$counter = 0;
$test->serverHandler(function($request, $response) use (&$counter) {
    $counter++;
    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setHeader('X-Request-Num', (string)$counter);
    $response->setBody("response-$counter");
});

/* Two pipelined requests sent in a single TCP write. The second uses
 * Connection: close so the server stops the keep-alive loop after it. */
$test->sendRequests([
    "GET /first HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "\r\n",
    "GET /second HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Connection: close\r\n" .
    "\r\n",
]);

$test->validate(function($status, $headers, $body, $rawResponse) {
    /* We expect exactly two complete HTTP responses concatenated. */
    $parts = preg_split('/(?=HTTP\/1\.1 )/', $rawResponse);
    $parts = array_values(array_filter($parts, fn($p) => $p !== ''));
    if (count($parts) !== 2) {
        throw new RuntimeException("Expected 2 responses, got " . count($parts));
    }
    foreach ([1, 2] as $i) {
        $idx = $i - 1;
        if (!str_contains($parts[$idx], "HTTP/1.1 200")) {
            throw new RuntimeException("Response #$i: missing 200 status");
        }
        if (stripos($parts[$idx], "x-request-num: $i") === false) {
            throw new RuntimeException("Response #$i: missing X-Request-Num: $i");
        }
        if (!str_contains($parts[$idx], "response-$i")) {
            throw new RuntimeException("Response #$i: missing body 'response-$i'");
        }
    }
});

$test->run();
--EXPECT--
OK
