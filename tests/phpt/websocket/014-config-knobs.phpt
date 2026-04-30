--TEST--
WebSocket: HttpServerConfig — ws_max_message_size / ws_max_frame_size / ws_ping_interval_ms / ws_pong_timeout_ms
--EXTENSIONS--
true_async_server
--FILE--
<?php
use TrueAsync\HttpServerConfig;

$c = new HttpServerConfig();

// Defaults per PLAN_WEBSOCKET.md §5.
echo "default msg:  ", $c->getWsMaxMessageSize(), "\n";
echo "default frm:  ", $c->getWsMaxFrameSize(), "\n";
echo "default ping: ", $c->getWsPingIntervalMs(), "\n";
echo "default pong: ", $c->getWsPongTimeoutMs(), "\n";

// Round-trip set/get.
$c->setWsMaxMessageSize(2 * 1024 * 1024)
  ->setWsMaxFrameSize(512 * 1024)
  ->setWsPingIntervalMs(10000)
  ->setWsPongTimeoutMs(5000);

echo "after  msg:   ", $c->getWsMaxMessageSize(), "\n";
echo "after  frm:   ", $c->getWsMaxFrameSize(), "\n";
echo "after  ping:  ", $c->getWsPingIntervalMs(), "\n";
echo "after  pong:  ", $c->getWsPongTimeoutMs(), "\n";

// Validation — under floor.
try {
    $c->setWsMaxMessageSize(0);
    echo "msg=0 NOT REJECTED\n";
} catch (\Throwable $e) {
    echo "msg=0 rejected: ", $e::class, "\n";
}
// Validation — over ceiling.
try {
    $c->setWsMaxFrameSize(1 << 30);   // 1 GiB > 256 MiB cap
    echo "frm=1GiB NOT REJECTED\n";
} catch (\Throwable $e) {
    echo "frm=1GiB rejected: ", $e::class, "\n";
}
// Ping interval 0 is allowed (means "disabled").
$c->setWsPingIntervalMs(0);
echo "ping=0 ok: ", $c->getWsPingIntervalMs(), "\n";

echo "Done\n";
--EXPECT--
default msg:  1048576
default frm:  1048576
default ping: 30000
default pong: 60000
after  msg:   2097152
after  frm:   524288
after  ping:  10000
after  pong:  5000
msg=0 rejected: TrueAsync\HttpServerInvalidArgumentException
frm=1GiB rejected: TrueAsync\HttpServerInvalidArgumentException
ping=0 ok: 0
Done
