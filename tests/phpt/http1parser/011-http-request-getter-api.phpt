--TEST--
HttpRequest: getter API surface — getHeader/getHeaderLine/getHeaders/hasHeader + content-type/length
--EXTENSIONS--
true_async_server
--FILE--
<?php
$body = "name=alice&age=30";

$req_str = "POST /a/b/c?x=1&y=2 HTTP/1.1\r\n"
         . "Host: example.test\r\n"
         . "User-Agent: testagent\r\n"
         . "Accept: text/html\r\n"
         . "Accept: application/json\r\n"  // multi-value
         . "Content-Type: application/x-www-form-urlencoded\r\n"
         . "Content-Length: " . strlen($body) . "\r\n"
         . "Connection: keep-alive\r\n"
         . "\r\n"
         . $body;

$req = TrueAsync\http_parse_request($req_str);

echo "method=" . $req->getMethod() . "\n";
echo "uri=" . $req->getUri() . "\n";
echo "ver=" . $req->getHttpVersion() . "\n";

// case-insensitive
echo "has_lower=" . ($req->hasHeader('host') ? 1 : 0) . "\n";
echo "has_mixed=" . ($req->hasHeader('Host') ? 1 : 0) . "\n";
echo "has_missing=" . ($req->hasHeader('X-Nope') ? 1 : 0) . "\n";

// getHeader on multi-value coalesces with ", "
echo "get_accept=" . var_export($req->getHeader('Accept'), true) . "\n";
echo "get_line=" . $req->getHeaderLine('Accept') . "\n";
echo "get_missing=" . var_export($req->getHeader('X-Nope'), true) . "\n";

$h = $req->getHeaders();
echo "header_count=" . count($h) . "\n";
echo "host=" . $req->getHeaderLine('Host') . "\n";

echo "ctype=" . var_export($req->getContentType(), true) . "\n";
echo "clen=" . var_export($req->getContentLength(), true) . "\n";
echo "keepalive=" . ($req->isKeepAlive() ? 1 : 0) . "\n";
echo "has_body=" . ($req->hasBody() ? 1 : 0) . "\n";
echo "body=" . $req->getBody() . "\n";

// getPost when body is form-urlencoded — note: behaviour depends on
// whether http_parse_request hydrates the form parser. Just check
// the API returns an array and don't assume content.
$post = $req->getPost();
echo "post_is_array=" . (is_array($post) ? 1 : 0) . "\n";

// getFiles on a non-multipart request returns an empty array
echo "files_is_array=" . (is_array($req->getFiles()) ? 1 : 0) . "\n";
echo "file_missing=" . var_export($req->getFile('nonexistent'), true) . "\n";
--EXPECT--
method=POST
uri=/a/b/c?x=1&y=2
ver=1.1
has_lower=1
has_mixed=1
has_missing=0
get_accept='text/html, application/json'
get_line=text/html, application/json
get_missing=NULL
header_count=6
host=example.test
ctype='application/x-www-form-urlencoded'
clen=17
keepalive=1
has_body=1
body=name=alice&age=30
post_is_array=1
files_is_array=1
file_missing=NULL
