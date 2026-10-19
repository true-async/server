--TEST--
HTTP/3: abort() after the peer ACKed the body sends the reset at once (reactor pool)
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
/* Under the pool the abort reaches the reactor as a mailbox wire, and the reset
 * leaves at the end of that mailbox drain. */

require __DIR__ . '/_h3_reset_after_ack.inc';

h3_reset_after_ack_run('096', 'abort');
?>
--EXPECTF--
%Apeer: reset in time%A
