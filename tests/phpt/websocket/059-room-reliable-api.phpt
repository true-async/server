--TEST--
Rooms reliable-send: API surface — RoomDeliveryException, send()/trySend() signatures, publish() returns array
--EXTENSIONS--
true_async_server
--FILE--
<?php

namespace TrueAsync;

/* Reliable room delivery adds send()/trySend() beside the best-effort publish(),
 * plus a DISTINCT RoomDeliveryException carrying how much of an at-least-once
 * send landed. It hangs off HttpServerException rather than WebSocketException,
 * because rooms are served by a build configured with --disable-websocket, where
 * that class does not exist. This asserts the shape only; behaviour with a
 * running worker is in 061/062. */

echo "RoomDeliveryException: ", class_exists(RoomDeliveryException::class) ? 'class' : 'missing', "\n";

// hierarchy: RoomDeliveryException -> HttpServerException -> Exception
$h = RoomDeliveryException::class;
while ($h !== false) {
    echo "  ", $h, "\n";
    $h = get_parent_class($h);
}

echo "distinct from backpressure: ",
     is_a(RoomDeliveryException::class, WebSocketBackpressureException::class, true) ? 'no' : 'yes', "\n";

// readonly int properties carrying the partial-delivery counts
$rc = new \ReflectionClass(RoomDeliveryException::class);
foreach (['delivered', 'pending'] as $p) {
    $rp = $rc->getProperty($p);
    echo "prop $p: ", (string) $rp->getType(), $rp->isReadOnly() ? ' readonly' : ' MUTABLE', "\n";
}

// method surface on both Room and HttpServer
foreach ([[Room::class, 'trySend'], [Room::class, 'send'], [HttpServer::class, 'trySend'], [HttpServer::class, 'send']] as [$c, $m]) {
    echo "$c::$m: ", method_exists($c, $m) ? 'yes' : 'no', "\n";
}

// return types: publish -> array breakdown; trySend -> bool; send -> int; WebSocket::publish stays int
echo "Room::publish returns ", (string) (new \ReflectionMethod(Room::class, 'publish'))->getReturnType(), "\n";
echo "HttpServer::publish returns ", (string) (new \ReflectionMethod(HttpServer::class, 'publish'))->getReturnType(), "\n";
echo "Room::trySend returns ", (string) (new \ReflectionMethod(Room::class, 'trySend'))->getReturnType(), "\n";
echo "Room::send returns ", (string) (new \ReflectionMethod(Room::class, 'send'))->getReturnType(), "\n";
echo "WebSocket::publish returns ", (string) (new \ReflectionMethod(WebSocket::class, 'publish'))->getReturnType(), "\n";

echo "Done\n";
--EXPECT--
RoomDeliveryException: class
  TrueAsync\RoomDeliveryException
  TrueAsync\HttpServerException
  Exception
distinct from backpressure: yes
prop delivered: int readonly
prop pending: int readonly
TrueAsync\Room::trySend: yes
TrueAsync\Room::send: yes
TrueAsync\HttpServer::trySend: yes
TrueAsync\HttpServer::send: yes
Room::publish returns array
HttpServer::publish returns array
Room::trySend returns bool
Room::send returns int
WebSocket::publish returns int
Done
