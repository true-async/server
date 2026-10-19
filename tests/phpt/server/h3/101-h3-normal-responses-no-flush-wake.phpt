--TEST--
HTTP/3: responses that end normally wake no deferred flush (in-thread listener)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=0
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
require __DIR__ . '/_h3_flush_wakes.inc';

h3_flush_wakes_run('101');
?>
--EXPECTF--
%A/plain: status 200, flush wakes 0
/stream: status 200, flush wakes 0
/file: status 200, flush wakes 0
after teardown: flush wakes 0%A
