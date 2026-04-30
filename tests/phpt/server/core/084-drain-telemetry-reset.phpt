--TEST--
HttpServer: resetTelemetry() zeros the 7 new drain counters
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Step 8 — resetTelemetry() is ops' escape hatch to wipe counters
 * between runs. Must cover the new drain counters; runtime state
 * (drain_epoch_current, drain_last_fired_ns) is deliberately NOT
 * zeroed because clearing mid-flight would confuse the control
 * loop. We verify both: counters reset, live state preserved. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19845 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setMaxConnections(1);   /* every accept trips hard-cap + drain */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* Fire a request to trigger the full drain cycle. */
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    stream_set_timeout($fp, 3);
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
    }
    fclose($fp);

    usleep(50000);

    $before = $server->getTelemetry();
    echo "before: reactive=",        (int)$before['drain_events_reactive_total'],
         " drained=",                 (int)$before['connections_drained_reactive_total'],
         " h1_close=",                (int)$before['h1_connection_close_sent_total'],
         " epoch=",                   (int)$before['drain_epoch_current'], "\n";

    $server->resetTelemetry();

    $after = $server->getTelemetry();
    echo "after: reactive=",         (int)$after['drain_events_reactive_total'],
         " drained=",                  (int)$after['connections_drained_reactive_total'],
         " h1_close=",                 (int)$after['h1_connection_close_sent_total'],
         /* drain_epoch_current is LIVE STATE — must NOT reset */
         " epoch_preserved=",          ($after['drain_epoch_current'] === $before['drain_epoch_current'] ? 1 : 0),
         "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
before: reactive=1 drained=1 h1_close=1 epoch=1
after: reactive=0 drained=0 h1_close=0 epoch_preserved=1
done
