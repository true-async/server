--TEST--
Rooms: a Room outlives the HttpServer that minted it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A room owns a reference to the topic hub, so releasing the HttpServer that
 * minted it leaves the handle working. A later step transfers a room into
 * another thread, and transferring a server would mean transferring its
 * listeners and its handler graph with it.
 *
 * WeakReference is the instrument for "released": it observes the PHP wrapper,
 * which is where the hub reference is dropped. That the hub itself stays alive
 * is a use-after-free question and needs valgrind or ASAN, not this file. */
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

/* Minting a room before start() enables rooms by itself: the handle takes a hub
 * reference, so returning one that could never publish would be the bug. */
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
