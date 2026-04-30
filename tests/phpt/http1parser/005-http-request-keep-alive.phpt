--TEST--
HttpRequest: Keep-Alive detection
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

// HTTP/1.1 - keep-alive by default
$req1 = \TrueAsync\http_parse_request(
    "GET / HTTP/1.1\r\n" .
    "Host: example.com\r\n" .
    "\r\n"
);

// HTTP/1.1 - explicit close
$req2 = \TrueAsync\http_parse_request(
    "GET / HTTP/1.1\r\n" .
    "Host: example.com\r\n" .
    "Connection: close\r\n" .
    "\r\n"
);

// HTTP/1.0 - close by default
$req3 = \TrueAsync\http_parse_request(
    "GET / HTTP/1.0\r\n" .
    "Host: example.com\r\n" .
    "\r\n"
);

// HTTP/1.0 - explicit keep-alive
$req4 = \TrueAsync\http_parse_request(
    "GET / HTTP/1.0\r\n" .
    "Host: example.com\r\n" .
    "Connection: keep-alive\r\n" .
    "\r\n"
);

var_dump($req1->getHttpVersion());
var_dump($req1->isKeepAlive());

var_dump($req2->getHttpVersion());
var_dump($req2->isKeepAlive());

var_dump($req3->getHttpVersion());
var_dump($req3->isKeepAlive());

var_dump($req4->getHttpVersion());
var_dump($req4->isKeepAlive());

?>
--EXPECT--
string(3) "1.1"
bool(true)
string(3) "1.1"
bool(false)
string(3) "1.0"
bool(false)
string(3) "1.0"
bool(true)
