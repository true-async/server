--TEST--
HTTP/1 parser: past max_input_nesting_level a query drops the name, a form is refused; values pass the input filter
--EXTENSIONS--
true_async_server
filter
--INI--
max_input_nesting_level=2
filter.default=special_chars
error_reporting=E_ALL & ~E_DEPRECATED
--FILE--
<?php
/* A query string keeps $_GET's rule: the name nested too deep is dropped and
 * the pairs after it are read. A form is refused instead (#318): a form
 * missing a field reads as another form. Every value goes through
 * sapi_module.input_filter, which ext/filter hooks with filter.default. */

$deep = 'x=1&a[b][c][d]=deep&q=%3Cb%3E&after=2';
$flat = 'x=1&q=%3Cb%3E&after=2';

$expected = [];
parse_str($deep, $expected);
echo json_encode($expected), "\n";
echo json_encode(TrueAsync\http_parse_request("GET /?$deep HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery()), "\n";

$urlencoded = static fn(string $body) => TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: application/x-www-form-urlencoded\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body);

$multipart = static function (array $fields) {
    $body = '';

    foreach ($fields as $name => $value) {
        $body .= "--bnd\r\nContent-Disposition: form-data; name=\"$name\"\r\n\r\n$value\r\n";
    }

    $body .= "--bnd--\r\n";

    return TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
        . "Content-Type: multipart/form-data; boundary=bnd\r\n"
        . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body);
};

$read = static function ($request): string {
    try {
        return json_encode($request->getPost());
    } catch (TrueAsync\HttpException $e) {
        return get_class($e) . ' ' . $e->getCode();
    }
};

echo $read($urlencoded($flat)), "\n";
echo $read($multipart(['x' => '1', 'q' => '<b>', 'after' => '2'])), "\n";
echo $read($urlencoded($deep)), "\n";
echo $read($multipart(['x' => '1', 'a[b][c][d]' => 'deep', 'after' => '2'])), "\n";
?>
--EXPECT--
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
TrueAsync\HttpException 400
TrueAsync\HttpException 400
