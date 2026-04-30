--TEST--
HttpRequest: Different HTTP methods
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

$methods = ['GET', 'POST', 'PUT', 'DELETE', 'HEAD', 'OPTIONS', 'PATCH'];

foreach ($methods as $method) {
    $request = \TrueAsync\http_parse_request(
        "$method /test HTTP/1.1\r\n" .
        "Host: example.com\r\n" .
        "\r\n"
    );
    var_dump($request->getMethod());
}

?>
--EXPECT--
string(3) "GET"
string(4) "POST"
string(3) "PUT"
string(6) "DELETE"
string(4) "HEAD"
string(7) "OPTIONS"
string(5) "PATCH"
