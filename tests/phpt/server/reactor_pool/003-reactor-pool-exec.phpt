--TEST--
Reactor pool exec (#80): reactor_pool_exec runs C init on each reactor's own thread
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_reactor_pool_exec_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Step 2a: prove the enabling primitive for putting transport on a reactor —
 * reactor_pool_exec posts a command that the reactor runs INLINE on its own
 * loop thread (this is where the H3 listener's uv-bound socket gets created in
 * the next step). The probe callback records the OS thread it ran on; the
 * summary must show every reactor ran it, off the parent thread, and on a
 * distinct thread per reactor. */
$reactors = 4;

$r = _http_server_reactor_pool_exec_selftest($reactors);

var_dump(is_array($r));
var_dump($r['reactors'] === $reactors);
var_dump($r['ran'] === $reactors);
var_dump($r['off_parent'] === $reactors);
var_dump($r['distinct_threads']);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
