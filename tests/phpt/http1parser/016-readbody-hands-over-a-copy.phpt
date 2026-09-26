--TEST--
HTTP/1 parser: readBody() on a buffered body returns it once and leaks no body-pool slot
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* The parser buffers a body in a body-pool slot. readBody() used to hand the
 * slot itself to PHP, which released it as a plain string and never returned
 * it to the pool: a debug build reports "Freeing ... (1048601 bytes)" from
 * body_pool.c at shutdown. */

$body    = str_repeat('0123456789', 100);
$request = TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\nContent-Type: text/plain\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body);

$first = $request->readBody();
echo 'first: ', $first === $body ? 'the body' : var_export($first, true), "\n";
echo 'second: ', var_export($request->readBody(), true), "\n";
?>
--EXPECT--
first: the body
second: NULL
