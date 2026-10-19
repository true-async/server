--TEST--
HTTP/3: a body past setMaxBodySize is answered 413 (#412)
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
/* A 200 KiB body against a 64 KiB cap. The handler waits in awaitBody() and
 * does not catch the refusal, so the HttpException 413 it is cancelled with
 * becomes the response. Before #412 the stream was only reset and the client
 * read no status. */

require __DIR__ . '/_h3_refused_body.inc';

$status = h3_refused_status('104', function ($req, $res) {
    $req->awaitBody();
    $res->setStatusCode(200)->setBody('unexpected');
}, ['max_body' => 64 * 1024, 'body_bytes' => 200 * 1024]);

echo "STATUS=", $status ?? 'none', "\n";
?>
--EXPECT--
STATUS=413
