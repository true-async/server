--TEST--
WebSocket: WebSocket / WebSocketMessage / WebSocketUpgrade are final, internal, and not user-constructible
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\WebSocket;
use TrueAsync\WebSocketMessage;
use TrueAsync\WebSocketUpgrade;

foreach ([WebSocket::class, WebSocketMessage::class, WebSocketUpgrade::class] as $c) {
    $r = new ReflectionClass($c);
    echo $c, ":\n";
    echo "  final:    ", $r->isFinal() ? 'yes' : 'no', "\n";
    echo "  internal: ", $r->isInternal() ? 'yes' : 'no', "\n";
    echo "  ctor:     ", $r->getConstructor()?->getName() ?? '<none>',
         ' / ', $r->getConstructor()?->isPrivate() ? 'private' : 'public', "\n";
}

// Private constructor must reject `new` from userland.
foreach ([WebSocket::class, WebSocketMessage::class, WebSocketUpgrade::class] as $c) {
    try {
        $r = new ReflectionClass($c);
        $r->newInstance();
        echo "$c: instantiation UNEXPECTEDLY succeeded\n";
    } catch (\Throwable $e) {
        echo "$c: blocked (", $e::class, ")\n";
    }
}

echo "Done\n";
--EXPECT--
TrueAsync\WebSocket:
  final:    yes
  internal: yes
  ctor:     __construct / private
TrueAsync\WebSocketMessage:
  final:    yes
  internal: yes
  ctor:     __construct / private
TrueAsync\WebSocketUpgrade:
  final:    yes
  internal: yes
  ctor:     __construct / private
TrueAsync\WebSocket: blocked (ReflectionException)
TrueAsync\WebSocketMessage: blocked (ReflectionException)
TrueAsync\WebSocketUpgrade: blocked (ReflectionException)
Done
