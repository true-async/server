--TEST--
E2E: $request->awaitBody() inside the handler coroutine (Phase 6 Step 4)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Phase 6 Step 4 regression: awaitBody() must work from inside the
 * handler coroutine. In the default auto_await_body=true path the body
 * is already fully materialised when the handler runs (dispatch is
 * still at message-complete), so awaitBody returns $this immediately
 * without suspending — but the code path that checks req->complete,
 * the fast-return, and the handler's ability to chain on it all have
 * to actually work end-to-end. */
require_once __DIR__ . '/HttpTestCase.php';

$test = new HttpTestCase();
$test->serverHandler(function($request, $response) {
    /* Call awaitBody() before touching the body — this is the
     * canonical pattern the Phase 6 plan describes for streaming
     * mode. Even though it's a no-op right now, the handler must
     * see $this back and have access to the buffered body. */
    $same = $request->awaitBody();
    if ($same !== $request) {
        $response->setStatusCode(500);
        $response->setBody('awaitBody did not return $this');
        return;
    }

    $body = $request->getBody();
    $response->setStatusCode(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setBody("got: {$body}");
});

$test->sendRequest(
    "POST /upload HTTP/1.1\r\n" .
    "Host: localhost\r\n" .
    "Content-Length: 13\r\n" .
    "Connection: close\r\n" .
    "\r\n" .
    "hello-awaited"
);

$test->expectStatus(200);
$test->expectBody('got: hello-awaited');

$test->run();
--EXPECT--
OK
