--TEST--
HTTP/3: an uncaught HttpException from a handler becomes its status (#412)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* The handler throws HttpException with code 404 on a plain GET. An uncaught
 * HttpException carries the status the server sends, as it does on HTTP/1 and
 * HTTP/2; HTTP/3 answered every uncaught exception with 500. */

require __DIR__ . '/_h3_refused_body.inc';

$status = h3_refused_status('108', function ($req, $res) {
    throw new TrueAsync\HttpException('no such thing', 404);
}, ['method' => 'GET', 'path' => '/missing']);

echo "STATUS=", $status ?? 'none', "\n";
?>
--EXPECT--
STATUS=404
