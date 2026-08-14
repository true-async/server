--TEST--
Rooms: a Room outlives the HttpServer that minted it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A room owns a reference to the topic hub, so releasing the HttpServer leaves
 * the handle usable. WeakReference observes the PHP wrapper only; whether the
 * hub itself is still there is a use-after-free question for valgrind. */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18068)
);
$server->enableRooms();

$room = $server->room('projects/demo');
$weak = WeakReference::create($server);

unset($server);

echo 'server released: ', $weak->get() === null ? 'yes' : 'no', "\n";
echo 'name: ', $room->name(), "\n";

$result = $room->publish('after-release');
echo 'served: ', $result['served'], ' posted: ', $result['posted'],
     ' dropped: ', $result['dropped'], "\n";

/* Minting a room before start() enables rooms by itself. */
$plain = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', 18068)
);
echo 'minted without enableRooms(): ', $plain->room('projects/other')->name(), "\n";

echo "Done\n";
--EXPECT--
server released: yes
name: projects/demo
served: 0 posted: 0 dropped: 0
minted without enableRooms(): projects/other
Done
