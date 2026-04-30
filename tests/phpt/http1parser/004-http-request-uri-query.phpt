--TEST--
HttpRequest: URI with query string
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

$request = \TrueAsync\http_parse_request(
    "GET /search?q=test&page=2&limit=10 HTTP/1.1\r\n" .
    "Host: example.com\r\n" .
    "\r\n"
);

var_dump($request->getUri());
var_dump($request->getMethod());

?>
--EXPECT--
string(30) "/search?q=test&page=2&limit=10"
string(3) "GET"
