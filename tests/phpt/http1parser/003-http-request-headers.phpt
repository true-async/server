--TEST--
HttpRequest: Multiple headers and case-insensitive lookup
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

$request = \TrueAsync\http_parse_request(
    "GET /test HTTP/1.1\r\n" .
    "Host: example.com\r\n" .
    "User-Agent: TestAgent/1.0\r\n" .
    "Accept: application/json\r\n" .
    "X-Custom-Header: custom-value\r\n" .
    "\r\n"
);

// Case-insensitive header lookup
var_dump($request->getHeader('host'));
var_dump($request->getHeader('HOST'));
var_dump($request->getHeader('Host'));
var_dump($request->getHeader('user-agent'));
var_dump($request->getHeader('x-custom-header'));

// Get all headers
$headers = $request->getHeaders();
var_dump(count($headers));
var_dump(isset($headers['host']));
var_dump(isset($headers['user-agent']));

// Non-existent header
var_dump($request->getHeader('non-existent'));

?>
--EXPECT--
string(11) "example.com"
string(11) "example.com"
string(11) "example.com"
string(13) "TestAgent/1.0"
string(12) "custom-value"
int(4)
bool(true)
bool(true)
NULL
