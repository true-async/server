--TEST--
request_wire round-trip (#80, D2): build flat wire on C side -> materialize HttpRequest
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_request_wire_roundtrip')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
$req = _http_server_request_wire_roundtrip(
    'POST',
    '/api/users?q=1',
    ['content-type' => 'application/json', 'accept' => '*/*'],
    '{"name":"x"}'
);

var_dump($req instanceof \TrueAsync\HttpRequest);
var_dump($req->getMethod());
var_dump($req->getUri());
var_dump($req->getHeader('content-type'));
var_dump($req->getHeader('accept'));
var_dump($req->getBody());
echo "done\n";
?>
--EXPECT--
bool(true)
string(4) "POST"
string(14) "/api/users?q=1"
string(16) "application/json"
string(3) "*/*"
string(12) "{"name":"x"}"
done
