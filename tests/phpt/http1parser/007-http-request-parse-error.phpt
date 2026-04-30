--TEST--
HttpRequest: Parse errors
--EXTENSIONS--
true_async_server
--FILE--
<?php

// Invalid request (no HTTP version)
$req1 = \TrueAsync\http_parse_request("GET /path\r\n\r\n");
var_dump($req1);

// Incomplete request (no final CRLF)
$req2 = \TrueAsync\http_parse_request("GET /path HTTP/1.1\r\nHost: example.com");
var_dump($req2);

// Empty string
$req3 = \TrueAsync\http_parse_request("");
var_dump($req3);

// Malformed
$req4 = \TrueAsync\http_parse_request("NOT A VALID REQUEST");
var_dump($req4);

// Dispose server state to prevent leak detector warnings
\TrueAsync\server_dispose();

?>
--EXPECT--
bool(false)
bool(false)
bool(false)
bool(false)
