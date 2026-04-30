--TEST--
HttpRequest: POST with body
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

$request = \TrueAsync\http_parse_request(
    "POST /api/users HTTP/1.1\r\n" .
    "Host: api.example.com\r\n" .
    "Content-Type: text/plain\r\n" .
    "Content-Length: 11\r\n" .
    "\r\n" .
    "Hello World"
);

var_dump($request->getMethod());
var_dump($request->getUri());
var_dump($request->hasBody());
var_dump($request->getBody());
var_dump($request->getHeader('content-type'));
var_dump($request->getHeader('Content-Length'));

?>
--EXPECT--
string(4) "POST"
string(10) "/api/users"
bool(true)
string(11) "Hello World"
string(10) "text/plain"
string(2) "11"
