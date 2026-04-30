--TEST--
Multipart: Simple field parsing
--EXTENSIONS--
true_async_server
--FILE--
<?php
$body = "-----boundary\r\n" .
        "Content-Disposition: form-data; name=\"field1\"\r\n" .
        "\r\n" .
        "value1\r\n" .
        "-----boundary--\r\n";

$request_str = "POST / HTTP/1.1\r\n" .
               "Host: test\r\n" .
               "Content-Type: multipart/form-data; boundary=---boundary\r\n" .
               "Content-Length: " . strlen($body) . "\r\n" .
               "\r\n" .
               $body;

$request = TrueAsync\http_parse_request($request_str);
var_dump($request->getPost());
?>
--EXPECT--
array(1) {
  ["field1"]=>
  string(6) "value1"
}
