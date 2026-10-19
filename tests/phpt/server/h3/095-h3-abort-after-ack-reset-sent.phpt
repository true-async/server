--TEST--
HTTP/3: abort() after the peer ACKed the body sends the reset at once (in-thread listener)
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
/* abort() queues RESET_STREAM from the handler coroutine, outside any UDP poll
 * tick, and the handler stays parked: no drain at handler exit sends it. */

require __DIR__ . '/_h3_reset_after_ack.inc';

h3_reset_after_ack_run('095', 'abort');
?>
--EXPECT--
peer: reset in time
