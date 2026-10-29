--TEST--
HttpRequest::getCookies(): the array php-cgi builds as $_COOKIE for the same header
--EXTENSIONS--
true_async_server
--FILE--
<?php

/* Each expected array is php-cgi's var_dump($_COOKIE) for HTTP_COOKIE set to
 * the same string: values rawurldecode()d with '+' kept, names as sent and
 * mangled, bracket arrays, the first plain duplicate kept, empty names and
 * blank pairs skipped, a planted __Host- key refused. */
$headers = [
    'sid=a%2Bb%3D%3D+x; theme=dark',
    'my.pref=1; prefs[a]=x; prefs[b]=y; ids[]=1; ids[]=2',
    'sess=good; sess=evil; =x;  ; flag',
    'sess=good; sess[x]=evil',
    '%61=1; 5=five; a[__Host-x]=1; __Host-id=2',
];

foreach ($headers as $header) {
    $r = \TrueAsync\http_parse_request("GET / HTTP/1.1\r\nHost: x\r\nCookie: $header\r\n\r\n");
    var_dump($r->getCookies());
}

?>
--EXPECT--
array(2) {
  ["sid"]=>
  string(7) "a+b==+x"
  ["theme"]=>
  string(4) "dark"
}
array(3) {
  ["my_pref"]=>
  string(1) "1"
  ["prefs"]=>
  array(2) {
    ["a"]=>
    string(1) "x"
    ["b"]=>
    string(1) "y"
  }
  ["ids"]=>
  array(2) {
    [0]=>
    string(1) "1"
    [1]=>
    string(1) "2"
  }
}
array(2) {
  ["sess"]=>
  string(4) "good"
  ["flag"]=>
  string(0) ""
}
array(1) {
  ["sess"]=>
  array(1) {
    ["x"]=>
    string(4) "evil"
  }
}
array(4) {
  ["%61"]=>
  string(1) "1"
  [5]=>
  string(4) "five"
  ["a"]=>
  array(0) {
  }
  ["__Host-id"]=>
  string(1) "2"
}
