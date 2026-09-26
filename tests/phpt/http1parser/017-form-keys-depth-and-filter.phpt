--TEST--
HTTP/1 parser: a name past max_input_nesting_level drops alone, and values pass the input filter, as parse_str() does
--EXTENSIONS--
true_async_server
filter
--INI--
max_input_nesting_level=2
filter.default=special_chars
error_reporting=E_ALL & ~E_DEPRECATED
--FILE--
<?php
/* The pairs after a name that nests too deep are still read, and every value
 * goes through sapi_module.input_filter, which ext/filter hooks with
 * filter.default: the query, an url-encoded body and multipart fields. */

$qs = 'x=1&a[b][c][d]=deep&q=%3Cb%3E&after=2';
$expected = [];
parse_str($qs, $expected);
echo json_encode($expected), "\n";

echo json_encode(TrueAsync\http_parse_request("GET /?$qs HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery()), "\n";

echo json_encode(TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: application/x-www-form-urlencoded\r\n"
    . 'Content-Length: ' . strlen($qs) . "\r\n\r\n" . $qs)->getPost()), "\n";

$body = '';

foreach (['x' => '1', 'a[b][c][d]' => 'deep', 'q' => '<b>', 'after' => '2'] as $name => $value) {
    $body .= "--bnd\r\nContent-Disposition: form-data; name=\"$name\"\r\n\r\n$value\r\n";
}

$body .= "--bnd--\r\n";

echo json_encode(TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: multipart/form-data; boundary=bnd\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body)->getPost()), "\n";
?>
--EXPECT--
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
