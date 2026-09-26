--TEST--
HTTP/1 parser: an url-encoded form past max_input_vars is refused, a query past it is cut as $_GET is
--EXTENSIONS--
true_async_server
--INI--
max_input_vars=3
--FILE--
<?php
$pairs = 'a=1&b=2&c=3&d=4';

echo json_encode(@TrueAsync\http_parse_request("GET /?$pairs HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery()), "\n";

$form = static fn(string $body) => TrueAsync\http_parse_request("POST / HTTP/1.1\r\nHost: t\r\n"
    . "Content-Type: application/x-www-form-urlencoded\r\n"
    . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body);

echo json_encode($form('a=1&b=2&c=3')->getPost()), "\n";

$request = $form($pairs);

foreach ([1, 2] as $attempt) {
    try {
        $request->getPost();
        echo "attempt $attempt: read\n";
    } catch (TrueAsync\HttpException $e) {
        echo "attempt $attempt: ", $e->getCode(), ' ', $e->getMessage(), "\n";
    }
}

echo 'body after the refusal: ', $request->getBody(), "\n";
?>
--EXPECT--
{"a":"1","b":"2","c":"3"}
{"a":"1","b":"2","c":"3"}
attempt 1: 400 the request form was refused: more fields than max_input_vars
attempt 2: 400 the request form was refused: more fields than max_input_vars
body after the refusal: a=1&b=2&c=3&d=4
