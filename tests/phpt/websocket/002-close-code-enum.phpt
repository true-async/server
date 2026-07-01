--TEST--
WebSocket: WebSocketCloseCode enum exposes the RFC 6455 §7.4.1 registry
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\WebSocketCloseCode;

// Verify each canonical close-code maps to the right integer per RFC 6455 §7.4.1.
$expected = [
    'NORMAL'                => 1000,
    'GOING_AWAY'            => 1001,
    'PROTOCOL_ERROR'        => 1002,
    'UNSUPPORTED_DATA'      => 1003,
    'NO_STATUS'             => 1005,
    'ABNORMAL_CLOSURE'      => 1006,
    'INVALID_FRAME_PAYLOAD' => 1007,
    'POLICY_VIOLATION'      => 1008,
    'MESSAGE_TOO_BIG'       => 1009,
    'MANDATORY_EXTENSION'   => 1010,
    'INTERNAL_SERVER_ERROR' => 1011,
    'TLS_HANDSHAKE'         => 1015,
];

foreach ($expected as $name => $value) {
    $case = constant(WebSocketCloseCode::class . '::' . $name);
    echo $case->name, '=', $case->value, ($case->value === $value ? ' ok' : ' WRONG'), "\n";
}

echo "cases: ", count(WebSocketCloseCode::cases()), "\n";

// from() round-trip
$c = WebSocketCloseCode::from(1009);
echo "from(1009): ", $c->name, "\n";

// Application-specific code (4000-4999) is intentionally NOT in the enum.
echo "tryFrom(4000): ", var_export(WebSocketCloseCode::tryFrom(4000), true), "\n";

echo "Done\n";
--EXPECT--
NORMAL=1000 ok
GOING_AWAY=1001 ok
PROTOCOL_ERROR=1002 ok
UNSUPPORTED_DATA=1003 ok
NO_STATUS=1005 ok
ABNORMAL_CLOSURE=1006 ok
INVALID_FRAME_PAYLOAD=1007 ok
POLICY_VIOLATION=1008 ok
MESSAGE_TOO_BIG=1009 ok
MANDATORY_EXTENSION=1010 ok
INTERNAL_SERVER_ERROR=1011 ok
TLS_HANDSHAKE=1015 ok
cases: 12
from(1009): MESSAGE_TOO_BIG
tryFrom(4000): NULL
Done
