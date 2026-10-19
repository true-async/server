--TEST--
HTTP/3: a body past setMaxBodySize is answered 413 under the reactor pool (#412)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* The same refusal as h3/104 with the listener on the reactor pool. The
 * refusal happens on the reactor thread, which has no handler to ask, so the
 * 413 has to come from the reactor itself. The reactor takes setMaxBodySize()
 * from the context the parent built for it. */

require __DIR__ . '/_h3_refused_body.inc';

$status = h3_refused_status('106', function ($req, $res) {
    $req->awaitBody();
    $res->setStatusCode(200)->setBody('unexpected');
}, ['max_body' => 64 * 1024, 'body_bytes' => 200 * 1024]);

echo "STATUS=", $status ?? 'none', "\n";
?>
--EXPECTF--
%ASTATUS=413%A
