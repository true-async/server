--TEST--
HTTP/3: a handler that throws after the peer ACKed the body resets the stream at once (reactor pool)
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
/* The reset leaves at the end of the reactor's mailbox drain that applies the
 * abort wire. */

require __DIR__ . '/_h3_reset_after_ack.inc';

h3_reset_after_ack_run('098', 'throw');
?>
--EXPECTF--
%Apeer: reset in time%A
