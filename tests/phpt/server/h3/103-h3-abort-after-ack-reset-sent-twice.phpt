--TEST--
HTTP/3: a second server on the same thread sends a reset queued outside a tick as the first did (in-thread listener)
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
/* The wake that sends output queued outside a tick belongs to the thread and
 * lives while a listener does: the first server's stop() disposes it, and the
 * second server's listener has to create it again. */

require __DIR__ . '/_h3_reset_after_ack.inc';

h3_reset_after_ack_run('103a', 'abort');
h3_reset_after_ack_run('103b', 'abort');
?>
--EXPECT--
peer: reset in time
peer: reset in time
