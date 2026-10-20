--TEST--
HTTP/1 parser: an url-encoded form and a query string past max_input_vars are both refused (#413)
--EXTENSIONS--
true_async_server
--INI--
max_input_vars=3
max_input_nesting_level=1
--FILE--
<?php
$pairs = 'a=1&b=2&c=3&d=4';

echo json_encode(TrueAsync\http_parse_request("GET /?a=1&b=2&c=3 HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery()), "\n";

$query = TrueAsync\http_parse_request("GET /p?$pairs HTTP/1.1\r\nHost: t\r\n\r\n");

foreach (['getQuery' => [], 'getQueryParam' => ['a']] as $getter => $args) {
    try {
        $query->$getter(...$args);
        echo "$getter: read\n";
    } catch (TrueAsync\HttpException $e) {
        echo "$getter: ", $e->getCode(), ' ', $e->getMessage(), "\n";
    }
}

echo 'path after the refusal: ', $query->getPath(), "\n";

try {
    TrueAsync\http_parse_request("GET /?a[b][c]=1 HTTP/1.1\r\nHost: t\r\n\r\n")->getQuery();
    echo "deep: read\n";
} catch (TrueAsync\HttpException $e) {
    echo 'deep: ', $e->getCode(), ' ', $e->getMessage(), "\n";
}

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
getQuery: 400 the query string was refused: more fields than max_input_vars
getQueryParam: 400 the query string was refused: more fields than max_input_vars
path after the refusal: /p
deep: 400 the query string was refused: a name nested deeper than max_input_nesting_level
{"a":"1","b":"2","c":"3"}
attempt 1: 400 the request form was refused: more fields than max_input_vars
attempt 2: 400 the request form was refused: more fields than max_input_vars
body after the refusal: a=1&b=2&c=3&d=4
