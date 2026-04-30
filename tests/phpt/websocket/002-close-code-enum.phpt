--TEST--
WebSocket: WebSocketCloseCode enum exposes the RFC 6455 §7.4.1 registry
--EXTENSIONS--
true_async_server
--FILE--
<?php

use TrueAsync\WebSocketCloseCode;

// Verify each canonical close-code maps to the right integer per RFC 6455 §7.4.1.
$expected = [
    'Normal'              => 1000,
    'GoingAway'           => 1001,
    'ProtocolError'       => 1002,
    'UnsupportedData'     => 1003,
    'NoStatus'            => 1005,
    'AbnormalClosure'     => 1006,
    'InvalidFramePayload' => 1007,
    'PolicyViolation'     => 1008,
    'MessageTooBig'       => 1009,
    'MandatoryExtension'  => 1010,
    'InternalServerError' => 1011,
    'TlsHandshake'        => 1015,
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
Normal=1000 ok
GoingAway=1001 ok
ProtocolError=1002 ok
UnsupportedData=1003 ok
NoStatus=1005 ok
AbnormalClosure=1006 ok
InvalidFramePayload=1007 ok
PolicyViolation=1008 ok
MessageTooBig=1009 ok
MandatoryExtension=1010 ok
InternalServerError=1011 ok
TlsHandshake=1015 ok
cases: 12
from(1009): MessageTooBig
tryFrom(4000): NULL
Done
