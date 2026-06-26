--TEST--
Persistent request lifecycle (#80, D7): reactor-domain http_request_t -> flag-aware accessors -> heap-clean free
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_persistent_request_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
// The reactor builds the request in the persistent (malloc) domain
// (method/uri/headers persistent, body ZMM as the worker command stream
// delivers it). The flag-aware accessors must deep-copy persistent strings
// into ZMM and rebuild the persistent headers table — never addref/dup
// persistent memory into a PHP value (the engine would efree malloc memory).
// The path with no query string also exercises the persistent-uri -> ZMM
// path copy. Under ASan this whole create -> read -> free cycle is heap-clean.
$req = _http_server_persistent_request_selftest(
    'POST',
    '/api/users',
    ['content-type' => 'application/json', 'accept' => '*/*', 'x-extension-header' => 'v'],
    '{"name":"x"}'
);

var_dump($req instanceof \TrueAsync\HttpRequest);
var_dump($req->getMethod());
var_dump($req->getUri());
var_dump($req->getPath());
var_dump($req->getHeader('content-type'));
var_dump($req->getHeader('x-extension-header'));
var_dump($req->getBody());

$headers = $req->getHeaders();
ksort($headers);
var_dump($headers);

// Drop the only ref -> free_obj -> http_request_destroy on the persistent domain.
unset($req);
echo "done\n";
?>
--EXPECT--
bool(true)
string(4) "POST"
string(10) "/api/users"
string(10) "/api/users"
string(16) "application/json"
string(1) "v"
string(12) "{"name":"x"}"
array(3) {
  ["accept"]=>
  string(3) "*/*"
  ["content-type"]=>
  string(16) "application/json"
  ["x-extension-header"]=>
  string(1) "v"
}
done
