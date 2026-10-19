--TEST--
HTTP/3: a refused body does not cut an early answer still in flight (#412)
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
/* The handler answers 403 at once with a 1 MiB body and never reads the upload:
 * its coroutine is gone, and its response decided but not out, when the 200 KiB
 * body runs past the 64 KiB cap. The refusal stops the upload and leaves the
 * response alone, so the client reads the status and the whole body. Before
 * #412 the stream was reset and the client read no status. */

require __DIR__ . '/_h3_refused_body.inc';

const BODY_SIZE = 1024 * 1024;

$status = h3_refused_status('109', function ($req, $res) {
    $res->setStatusCode(403)->setBody(str_repeat('ABCDEFGHIJKLMNOP', BODY_SIZE / 16));
}, ['max_body' => 64 * 1024, 'body_bytes' => 200 * 1024], $body_len);

echo "STATUS=", $status ?? 'none', "\n";
echo "received=", $body_len ?? 'none', " of ", BODY_SIZE, "\n";
?>
--EXPECT--
STATUS=403
received=1048576 of 1048576
