--TEST--
Reactor pool substrate (#80): spawn N reactors, drain their #81 inbound, clean shutdown
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_reactor_pool_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
$reactors = 3;
$items    = 2000;

$processed = _http_server_reactor_pool_selftest($reactors, $items);

var_dump(is_array($processed));
var_dump(count($processed) === $reactors);

$all_drained = true;
foreach ($processed as $idx => $n) {
    if ($n !== $items) {
        $all_drained = false;
        echo "reactor {$idx}: drained {$n}, expected {$items}\n";
    }
}
var_dump($all_drained);

echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
done
