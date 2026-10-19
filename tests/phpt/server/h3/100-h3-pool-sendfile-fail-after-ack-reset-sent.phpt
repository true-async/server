--TEST--
HTTP/3: a sendFile() read that fails after the peer ACKed the sent chunks resets the stream at once (reactor pool)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
if (!function_exists('_http_fault_enable')) die('skip fault injection not compiled in');
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
require __DIR__ . '/_h3_sendfile_fail_after_ack.inc';

h3_sendfile_fail_after_ack_run('100');
?>
--EXPECTF--
%Apump fault=1
peer: reset in time, body incomplete%A
