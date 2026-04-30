--TEST--
HttpRequest: awaitBody() + safe teardown with aliased references
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Phase 6 Step 4 infra: awaitBody() is the Phase-6-correct name for
 * "wait until the request body is fully received". Because dispatch
 * still happens at message-complete (body already materialised), this
 * call is a no-op and returns $this without suspending.
 *
 * This test also regressions a pre-existing shutdown-ordering bug:
 * fast-shutdown used to free HttpRequest objects AFTER RSHUTDOWN had
 * torn down the parser pool, crashing parser_pool_return when
 * `parsers` was already NULL. Having two live refs to the request at
 * end-of-script is what triggers the fast-shutdown path.
 */

$request = TrueAsync\http_parse_request(
    "POST /upload HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello"
);

var_dump(method_exists($request, 'awaitBody'));
var_dump(method_exists($request, 'await'));  /* old name must be gone */

$same = $request->awaitBody();
var_dump($same === $request);
var_dump($same->getBody());

/* Intentionally leave both $request and $same alive until script end
 * so shutdown exercises the parser-pool-after-teardown path. */
?>
--EXPECT--
bool(true)
bool(false)
bool(true)
string(5) "hello"
