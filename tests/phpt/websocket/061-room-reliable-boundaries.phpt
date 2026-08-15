--TEST--
Rooms reliable-send: no-coroutine send() refuses (NO_CONTEXT); trySend()/publish()/stats boundary contracts
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RoomDeliveryException;

/* These boundary contracts are reachable synchronously — rooms enabled, but no
 * worker attached to this thread and no other worker running:
 *   - send() outside a coroutine must THROW (it does NOT degrade to best-effort
 *     publish() — the one caller who chose the reliable path must not silently get
 *     lossy semantics), and the exception carries delivered/pending;
 *   - trySend() needs no coroutine, and with no thread attached to the hub it
 *     reports false: there was nothing to deliver to and no later attach can
 *     rescue the message, so "nothing was parked" is the honest answer;
 *   - publish() returns its {served, posted, dropped, workers} breakdown, where
 *     workers=0 is what separates "nowhere to go" from "the room is empty";
 *   - a wildcard topic is rejected on the reliable path as it is on publish();
 *   - the reliable-path retry_* counters are exposed by getRuntimeStats(). */
$s = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', 34110));
$s->enableRooms();

try {
    $s->send('ops/room', 'hi');
    echo "no-coroutine send: NOT THROWN\n";
} catch (RoomDeliveryException $e) {
    echo "no-coroutine send throws RoomDeliveryException; delivered=", $e->delivered, " pending=", $e->pending, "\n";
}

var_dump($s->trySend('ops/room', 'hi'));

$p = $s->publish('ops/room', 'hi');
echo "publish keys: ", implode(',', array_keys($p)), "\n";
echo "publish served/posted/dropped/workers: {$p['served']}/{$p['posted']}/{$p['dropped']}/{$p['workers']}\n";

try {
    $s->send('ops/#', 'hi');
    echo "wildcard send: NOT REJECTED\n";
} catch (RoomDeliveryException $e) {
    echo "wildcard send: wrong exception (", $e::class, ")\n";
} catch (\Throwable $e) {
    echo "wildcard send rejected: ", $e::class, "\n";
}

$st = $s->getRuntimeStats();
$keys = array_values(array_filter(array_keys($st), fn($k) => str_starts_with($k, 'ws_retry')));
sort($keys);
echo "retry stat keys: ", implode(',', $keys), "\n";

echo "Done\n";
--EXPECT--
no-coroutine send throws RoomDeliveryException; delivered=0 pending=0
bool(false)
publish keys: served,posted,dropped,workers
publish served/posted/dropped/workers: 0/0/0/0
wildcard send rejected: TrueAsync\HttpServerInvalidArgumentException
retry stat keys: ws_retry_delivered,ws_retry_expired,ws_retry_gone,ws_retry_queued,ws_retry_rejected,ws_retry_shutdown
Done
