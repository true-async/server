--TEST--
HttpRequest: Simple GET request
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

$request = \TrueAsync\http_parse_request(
    "GET /path HTTP/1.1\r\n" .
    "Host: example.com\r\n" .
    "\r\n"
);

var_dump($request instanceof HttpRequest);
var_dump($request->getMethod());
var_dump($request->getUri());
var_dump($request->getHttpVersion());
var_dump($request->getHeader('host'));
var_dump($request->hasBody());
var_dump($request->isKeepAlive());

?>
--EXPECT--
bool(true)
string(3) "GET"
string(5) "/path"
string(3) "1.1"
string(11) "example.com"
bool(false)
bool(true)
