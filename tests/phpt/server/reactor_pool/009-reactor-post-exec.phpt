--TEST--
Reactor pool post-exec (#80, D8/B4): fire-and-forget callbacks run on the reactor, caller never blocks
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_reactor_post_exec_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* The worker->reactor reverse path (D8/B4) reuses the reactor's existing inbound
 * channel — there is no separate mailbox. It just needs a *non-blocking* exec:
 * reactor_pool_exec waits for a completion ack (wrong for a path where nobody
 * blocks), so reactor_pool_post_exec posts fire-and-forget. This drives that
 * primitive: post N callbacks into each reactor without ever blocking, then
 * confirm every one ran on the reactor's own thread (off the parent). The tagged
 * response/consumed/cancel message rides on top of this as payload — that lands
 * with the live reverse path. */

use function Async\spawn;
use function Async\await;

$reactors = 3;
$count    = 40;

$r = await(spawn(fn () => _http_server_reactor_post_exec_selftest($reactors, $count)));

var_dump(is_array($r));
var_dump($r['reactors'] === $reactors);
var_dump($r['expected'] === $reactors * $count);
var_dump($r['ran'] === $reactors * $count);     /* every fire-and-forget callback ran */
var_dump($r['off_parent'] === $reactors);       /* all on reactor threads, not the caller */

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
