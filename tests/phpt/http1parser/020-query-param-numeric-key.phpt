--TEST--
HttpRequest::getQueryParam(): a numeric name finds the integer key getQuery() holds
--EXTENSIONS--
true_async_server
--FILE--
<?php

$r = \TrueAsync\http_parse_request("GET /?5=five&07=seven&-3=minus HTTP/1.1\r\nHost: x\r\n\r\n");

var_dump(array_keys($r->getQuery()));
var_dump($r->getQueryParam('5'));
var_dump($r->getQueryParam('07'));
var_dump($r->getQueryParam('-3'));
var_dump($r->getQueryParam('6', 'none'));

?>
--EXPECT--
array(3) {
  [0]=>
  int(5)
  [1]=>
  string(2) "07"
  [2]=>
  int(-3)
}
string(4) "five"
string(5) "seven"
string(5) "minus"
string(4) "none"
