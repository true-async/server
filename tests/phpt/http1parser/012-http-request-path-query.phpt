--TEST--
HttpRequest: getPath / getQuery / getQueryParam — lazy URI parsing
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\HttpRequest;

/* Helper: parse a raw HTTP/1.1 request and return the HttpRequest object */
function req(string $raw): HttpRequest {
    return \TrueAsync\http_parse_request($raw);
}

/* ── 1. Basic path/query split ─────────────────────────────────────── */
$r = req("GET /search?q=hello&page=2 HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getPath());
var_dump($r->getQuery());
var_dump($r->getQueryParam('q'));
var_dump($r->getQueryParam('page'));

/* ── 2. No query string at all ──────────────────────────────────────── */
$r = req("GET /about HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getPath());
var_dump($r->getQuery());

/* ── 3. Trailing '?' with empty query ───────────────────────────────── */
$r = req("GET /empty? HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getPath());
var_dump($r->getQuery());

/* ── 4. Percent-encoding and '+' as space ───────────────────────────── */
$r = req("GET /find?term=hello+world&tag=caf%C3%A9 HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getQueryParam('term'));
var_dump($r->getQueryParam('tag'));

/* ── 5. PHP array notation: foo[] and foo[bar] ──────────────────────── */
$r = req("GET /?ids%5B%5D=1&ids%5B%5D=2&user%5Bname%5D=bob HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getQueryParam('ids'));
var_dump($r->getQueryParam('user'));

/* ── 6. getQueryParam default value ────────────────────────────────── */
$r = req("GET /path?existing=yes HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getQueryParam('missing'));
var_dump($r->getQueryParam('missing', 'fallback'));
var_dump($r->getQueryParam('missing', 42));
var_dump($r->getQueryParam('existing', 'fallback'));

/* ── 7. getPath strips query, getUri retains it ─────────────────────── */
$r = req("GET /api/v1/users?sort=asc HTTP/1.1\r\nHost: x\r\n\r\n");
var_dump($r->getPath());
var_dump($r->getUri());

/* ── 8. Cache: calling getPath/getQuery multiple times is consistent ── */
$r = req("GET /cache?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n");
$q1 = $r->getQuery();
$q2 = $r->getQuery();
var_dump($q1 === $q2);
var_dump($r->getPath() === $r->getPath());

?>
--EXPECT--
string(7) "/search"
array(2) {
  ["q"]=>
  string(5) "hello"
  ["page"]=>
  string(1) "2"
}
string(5) "hello"
string(1) "2"
string(6) "/about"
array(0) {
}
string(6) "/empty"
array(0) {
}
string(11) "hello world"
string(5) "café"
array(2) {
  [0]=>
  string(1) "1"
  [1]=>
  string(1) "2"
}
array(1) {
  ["name"]=>
  string(3) "bob"
}
NULL
string(8) "fallback"
int(42)
string(3) "yes"
string(13) "/api/v1/users"
string(22) "/api/v1/users?sort=asc"
bool(true)
bool(true)
