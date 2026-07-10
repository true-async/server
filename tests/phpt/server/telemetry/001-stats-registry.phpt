--TEST--
Stats registry (#5, A1): per-worker counter slab claim/retire/recycle
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_stats_registry_selftest')) {
    die('skip built without --enable-http-server-test-hooks');
}
?>
--FILE--
<?php
/* Stage A1: the stats registry is a contiguous slab of per-worker counter slots.
 * The hook drives it with no server or coroutine — pure C — claiming every slot,
 * proving a full slab refuses further claims, round-tripping a counter through a
 * slot, and retiring + recycling a slot (counters zeroed on reclaim). */

function check(int $cap): void {
    $r = _http_server_stats_registry_selftest($cap);

    $ok = $r['capacity'] === $cap
        && $r['claimed'] === $cap
        && $r['distinct'] === true
        && $r['overflow_refused'] === true
        && $r['count_full'] === $cap
        && $r['write_read_ok'] === true
        && $r['active0'] === true
        && $r['retire_ok'] === true
        && $r['count_retired'] === $cap - 1
        && $r['inactive0'] === true
        && $r['recycle_idx_ok'] === true
        && $r['recycled_zeroed'] === true;

    echo "cap=$cap ", $ok ? "PASS" : "FAIL", "\n";
    if (!$ok) {
        var_dump($r);
    }
}

check(4);   /* multi-slot pool */
check(1);   /* slab-of-1 (single-worker / non-pool) */

echo "done\n";
?>
--EXPECT--
cap=4 PASS
cap=1 PASS
done
