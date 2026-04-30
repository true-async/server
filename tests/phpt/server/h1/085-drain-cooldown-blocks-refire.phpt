--TEST--
HttpServer: drain cooldown suppresses rapid re-triggers
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Step 8 — cooldown prevents oscillation when pause flips on/off
 * rapidly. Two quick hard-cap events (max_connections=1) within the
 * cooldown window: the first bumps drain_events_reactive_total; the
 * second is blocked and increments drain_events_cooldown_blocked_total
 * instead. The epoch stays at 1 (not bumped by the blocked event). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19846 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setMaxConnections(1)
    ->setDrainCooldownMs(10000);   /* 10 s — large window so the refire
                                      inside our ~200 ms test falls
                                      squarely inside it */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* First connection — accept trips hard-cap → drain epoch=1. */
    $fp1 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    fwrite($fp1, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    stream_set_timeout($fp1, 3);
    while (!feof($fp1)) {
        $c = fread($fp1, 4096); if ($c === '' || $c === false) break;
    }
    fclose($fp1);

    /* Let conn1 fully close + listener resume before attempting the
     * second connect. The resume is driven by active_connections
     * dropping below pause_low. */
    usleep(100000);

    /* Second connection — accept trips hard-cap again. Inside
     * trigger_drain, the cooldown check sees drain_last_fired_ns is
     * ~100 ms ago (< 10 000 ms), blocks the re-trigger, bumps
     * drain_events_cooldown_blocked_total. Epoch stays 1.
     *
     * Because epoch did NOT advance, fp2 receives the response on a
     * keep-alive connection (no `Connection: close`). Use `Connection:
     * close` in the request so the server tears down after one response
     * and our read loop bounds — without this the loop blocks forever
     * waiting for the next keep-alive request that will never come. */
    $fp2 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    fwrite($fp2, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    stream_set_timeout($fp2, 3);
    while (!feof($fp2)) {
        $c = fread($fp2, 4096); if ($c === '' || $c === false) break;
    }
    fclose($fp2);

    usleep(100000);

    $tel = $server->getTelemetry();
    echo "epoch=",                 (int)$tel['drain_epoch_current'], "\n";
    echo "reactive_events=",       (int)$tel['drain_events_reactive_total'], "\n";
    echo "cooldown_blocked=",      (int)$tel['drain_events_cooldown_blocked_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
epoch=1
reactive_events=1
cooldown_blocked=1
done
