--TEST--
HttpRequest: trailers never change original headers or leak through parser reuse
--EXTENSIONS--
true_async_server
--FILE--
<?php
$prefix = "POST / HTTP/1.1\r\nHost: original\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n";
foreach ([
    "Authorization: Bearer trailer\r\n",
    "Authorization: Bearer trailer\r\nX-Last: discarded\r\n",
    "Host: trailer\r\nContent-Type: application/json\r\nX-Last: discarded\r\n",
] as $trailers) {
    $request = TrueAsync\http_parse_request($prefix . $trailers . "\r\n");
    var_dump($request->getHeader('host'));
    var_dump($request->getHeader('authorization'));
    var_dump($request->getHeader('content-type'));
    var_dump($request->getHeader('x-last'));
    var_dump(count($request->getHeaders()));
    unset($request);
    $next = TrueAsync\http_parse_request("GET /next HTTP/1.1\r\nHost: next\r\n\r\n");
    var_dump($next->getHeader('host'), count($next->getHeaders()));
    unset($next);
}
TrueAsync\server_dispose();
?>
--EXPECT--
string(8) "original"
NULL
NULL
NULL
int(2)
string(4) "next"
int(1)
string(8) "original"
NULL
NULL
NULL
int(2)
string(4) "next"
int(1)
string(8) "original"
NULL
NULL
NULL
int(2)
string(4) "next"
int(1)
