--TEST--
WebSocket: all PHP classes and the close-code enum are registered
--EXTENSIONS--
true_async_server
--FILE--
<?php

namespace TrueAsync;

$classes = [
    WebSocket::class,
    WebSocketMessage::class,
    WebSocketUpgrade::class,
    WebSocketException::class,
    WebSocketClosedException::class,
    WebSocketBackpressureException::class,
    WebSocketConcurrentReadException::class,
];

foreach ($classes as $c) {
    echo $c, ': ', class_exists($c, false) ? 'class' : 'missing', "\n";
}

echo WebSocketCloseCode::class, ': ',
     enum_exists(WebSocketCloseCode::class, false) ? 'enum' : 'missing', "\n";

// Exception hierarchy: WebSocketClosedException -> WebSocketException -> HttpServerException -> Exception
$h = WebSocketClosedException::class;
while ($h !== false) {
    echo "  ", $h, "\n";
    $h = get_parent_class($h);
}

echo "Done\n";
--EXPECT--
TrueAsync\WebSocket: class
TrueAsync\WebSocketMessage: class
TrueAsync\WebSocketUpgrade: class
TrueAsync\WebSocketException: class
TrueAsync\WebSocketClosedException: class
TrueAsync\WebSocketBackpressureException: class
TrueAsync\WebSocketConcurrentReadException: class
TrueAsync\WebSocketCloseCode: enum
  TrueAsync\WebSocketClosedException
  TrueAsync\WebSocketException
  TrueAsync\HttpServerException
  Exception
Done
