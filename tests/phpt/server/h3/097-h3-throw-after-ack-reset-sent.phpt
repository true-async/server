--TEST--
HTTP/3: a handler that throws after the peer ACKed the body resets the stream at once (in-thread listener)
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
/* The reset goes out with the drain that follows the handler's disposal. */

require __DIR__ . '/_h3_reset_after_ack.inc';

h3_reset_after_ack_run('097', 'throw');
?>
--EXPECTF--
Warning: [true-async-server] uncaught exception in http handler: RuntimeException: handler failed after the peer ACKed its body (in %s:%d) in Unknown on line 0
peer: reset in time
