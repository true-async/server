--TEST--
HttpRequest: Extension methods (WebDAV) take the fallback path but still parse
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

/* The 9 RFC 9110 methods are served from an interned-string pool
 * (zero-alloc). Extension methods recognised by llhttp — WebDAV
 * PROPFIND/MKCOL, RTSP ANNOUNCE, etc. — fall through to the
 * zend_string_init path. Both must round-trip to PHP identically. */

$cases = [
    // (method, expected length)
    ['GET',      3],  // interned
    ['POST',     4],  // interned
    ['OPTIONS',  7],  // interned (CONNECT is rejected on an origin server, #47)
    ['PROPFIND', 8],  // fallback (WebDAV)
    ['MKCOL',    5],  // fallback (WebDAV)
    ['REPORT',   6],  // fallback
];

foreach ($cases as [$method, $len]) {
    $request = \TrueAsync\http_parse_request(
        "$method /r HTTP/1.1\r\nHost: x\r\n\r\n"
    );
    $got = $request->getMethod();
    $ok  = ($got === $method && strlen($got) === $len) ? 'ok' : 'FAIL';
    echo "$method $ok\n";
}

?>
--EXPECT--
GET ok
POST ok
OPTIONS ok
PROPFIND ok
MKCOL ok
REPORT ok
