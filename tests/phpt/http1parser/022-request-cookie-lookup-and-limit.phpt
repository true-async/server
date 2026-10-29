--TEST--
HttpRequest::getCookie(): lookup, default, no header, joined headers, max_input_vars keeps the first cookies
--EXTENSIONS--
true_async_server
--INI--
max_input_vars=2
--FILE--
<?php

function req(string $headers): \TrueAsync\HttpRequest {
    return \TrueAsync\http_parse_request("GET / HTTP/1.1\r\nHost: x\r\n{$headers}\r\n");
}

/* No Cookie header */
$r = req("");
var_dump($r->getCookies(), $r->getCookie('a'), $r->getCookie('a', 'none'));

/* Two Cookie headers are read as one list; a numeric name is found by its string */
$r = req("Cookie: 5=five\r\nCookie: my.pref=on\r\n");
var_dump($r->getCookie('5'), $r->getCookie('my_pref'), $r->getCookie('my.pref', 'unmangled'));

/* Past max_input_vars the first cookies are kept and the request is not refused */
$r = req("Cookie: a=1; b=2; c=3\r\n");
var_dump($r->getCookies(), $r->getCookie('c', 'cut'));

/* Built once: the second call returns the same array */
var_dump($r->getCookies() === $r->getCookies());

?>
--EXPECT--
array(0) {
}
NULL
string(4) "none"
string(4) "five"
string(2) "on"
string(9) "unmangled"
array(2) {
  ["a"]=>
  string(1) "1"
  ["b"]=>
  string(1) "2"
}
string(3) "cut"
bool(true)
