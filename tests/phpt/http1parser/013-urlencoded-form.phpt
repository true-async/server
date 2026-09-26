--TEST--
HTTP/1 parser: an url-encoded body fills getPost() the way PHP fills $_POST
--EXTENSIONS--
true_async_server
--FILE--
<?php
$parse = static function (string $content_type, string $body): TrueAsync\HttpRequest {
    return TrueAsync\http_parse_request(
        "POST /form?q=1 HTTP/1.1\r\nHost: t\r\n"
        . "Content-Type: $content_type\r\n"
        . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body
    );
};

$form = 'a=1&list%5B%5D=2&list%5B%5D=3&map%5Bkey%5D=4&two+words=x+y&m[0][1]=z&empty=';

/* Media type and parameter case do not matter; the charset parameter is allowed. */
$request = $parse('Application/X-WWW-Form-Urlencoded; Charset=UTF-8', $form);
var_dump($request->getPost());
var_dump($request->getQuery());
var_dump($request->getFiles());

/* The same bytes under another media type are not a form. */
var_dump($parse('text/plain', $form)->getPost());
var_dump($parse('application/x-www-form-urlencoded-not', $form)->getPost());

/* Reading the body first leaves the form readable. */
$request = $parse('application/x-www-form-urlencoded', 'x=1&y=2');
var_dump($request->readBody(), $request->readBody(), $request->getPost());
?>
--EXPECT--
array(6) {
  ["a"]=>
  string(1) "1"
  ["list"]=>
  array(2) {
    [0]=>
    string(1) "2"
    [1]=>
    string(1) "3"
  }
  ["map"]=>
  array(1) {
    ["key"]=>
    string(1) "4"
  }
  ["two_words"]=>
  string(3) "x y"
  ["m"]=>
  array(1) {
    [0]=>
    array(1) {
      [1]=>
      string(1) "z"
    }
  }
  ["empty"]=>
  string(0) ""
}
array(1) {
  ["q"]=>
  string(1) "1"
}
array(0) {
}
array(0) {
}
array(0) {
}
string(7) "x=1&y=2"
NULL
array(2) {
  ["x"]=>
  string(1) "1"
  ["y"]=>
  string(1) "2"
}
