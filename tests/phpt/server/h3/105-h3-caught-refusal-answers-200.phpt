--TEST--
HTTP/3: a handler that catches the refusal HttpException answers with its own status (#412)
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
/* A 200 KiB body against a 64 KiB cap. The handler catches the HttpException
 * 413 from awaitBody() and answers 200. The response the handler decided is
 * the one the client reads, not the refusal's 413. */

require __DIR__ . '/_h3_refused_body.inc';

$status = h3_refused_status('105', function ($req, $res) {
    try {
        $req->awaitBody();
        $res->setStatusCode(500)->setBody('not refused');
    } catch (TrueAsync\HttpException $e) {
        $res->setStatusCode(200)->setBody('caught');
    }
}, ['max_body' => 64 * 1024, 'body_bytes' => 200 * 1024]);

echo "STATUS=", $status ?? 'none', "\n";
?>
--EXPECT--
STATUS=200
