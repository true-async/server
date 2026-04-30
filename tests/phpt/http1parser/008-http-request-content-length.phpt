--TEST--
HttpRequest: content_length field survives pool reuse (Phase 6 merge)
--EXTENSIONS--
true_async_server
--FILE--
<?php
/* Phase 6 kept content_length on the merged http_request_t. This test
 * verifies the getContentLength() accessor still reads the right field
 * after the struct reshuffle, and that a subsequent (pool-reused) parse
 * starts fresh. */

$body1 = str_repeat('A', 10);
$req1 = TrueAsync\http_parse_request(
    "POST /a HTTP/1.1\r\nHost: t\r\nContent-Length: " . strlen($body1) . "\r\n\r\n" . $body1
);
var_dump($req1->getContentLength());
var_dump(strlen($req1->getBody()));
unset($req1);

$body2 = str_repeat('B', 3);
$req2 = TrueAsync\http_parse_request(
    "POST /b HTTP/1.1\r\nHost: t\r\nContent-Length: " . strlen($body2) . "\r\n\r\n" . $body2
);
var_dump($req2->getContentLength());
var_dump($req2->getBody());

/* GET without a body: content_length must be 0 / header absent */
$req3 = TrueAsync\http_parse_request("GET /c HTTP/1.1\r\nHost: t\r\n\r\n");
var_dump($req3->getContentLength());
var_dump($req3->getBody());
?>
--EXPECT--
int(10)
int(10)
int(3)
string(3) "BBB"
NULL
string(0) ""
