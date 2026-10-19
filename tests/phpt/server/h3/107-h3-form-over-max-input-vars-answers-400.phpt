--TEST--
HTTP/3: a multipart form past max_input_vars is answered 400 (#412)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--INI--
max_input_vars=5
--FILE--
<?php
/* Six fields against max_input_vars=5. The handler calls getPost() and does
 * not catch the HttpException 400 it throws, so that exception becomes the
 * response. The status was 500 before #412. h3/073 holds the getter side. */

require __DIR__ . '/_h3_refused_body.inc';

$boundary = 'h3bnd';
$body = implode('', array_map(
    static fn($i) => "--$boundary\r\nContent-Disposition: form-data; name=\"k$i\"\r\n\r\nv\r\n",
    range(1, 6))) . "--$boundary--\r\n";

$status = h3_refused_status('107', function ($req, $res) {
    $req->getPost();
    $res->setStatusCode(200)->setBody('unexpected');
}, ['path' => '/form', 'body' => $body, 'header' => "content-type: multipart/form-data; boundary=$boundary"]);

echo "STATUS=", $status ?? 'none', "\n";
?>
--EXPECT--
STATUS=400
