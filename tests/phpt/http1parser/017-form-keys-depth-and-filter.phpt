--TEST--
HTTP/1 parser: past max_input_nesting_level a query and a form are refused; values pass the input filter
--EXTENSIONS--
true_async_server
filter
--INI--
max_input_nesting_level=2
filter.default=special_chars
error_reporting=E_ALL & ~E_DEPRECATED
--FILE--
<?php
/* $_GET drops the name nested too deep and reads the pairs after it (the
 * parse_str line). The server refuses it instead, a query string (#413) as a
 * form (#318): a query or form missing a field reads as another one. Every
 * value goes through sapi_module.input_filter, which ext/filter hooks with
 * filter.default. */

$deep = 'x=1&a[b][c][d]=deep&q=%3Cb%3E&after=2';
$flat = 'x=1&q=%3Cb%3E&after=2';

$expected = [];
parse_str($deep, $expected);
echo json_encode($expected), "\n";
echo json_encode(TrueAsync\http_parse_request("GET /?$flat HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery()), "\n";

try {
    TrueAsync\http_parse_request("GET /?$deep HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery();
    echo "deep query: read\n";
} catch (TrueAsync\HttpException $e) {
    echo 'deep query: ', get_class($e), ' ', $e->getCode(), "\n";
}

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
deep query: TrueAsync\HttpException 400
{"x":"1","q":"&#60;b&#62;","after":"2"}
{"x":"1","q":"&#60;b&#62;","after":"2"}
TrueAsync\HttpException 400
TrueAsync\HttpException 400
