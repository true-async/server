--TEST--
HttpServer: hard-cap transition drains active HTTP/1 keep-alive connections
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Step 8 — reactive drain. Hard-cap transition (active_connections >=
 * max_connections) bumps drain_epoch_current inside pause_listeners;
 * already-accepted connections pick it up on their next response and
 * emit `Connection: close`. Spread window is wired but kept short for
 * this test so the phpt doesn't run for 5 s. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19841 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(2)             /* hard-cap triggers at 2 accepts */
    ->setDrainSpreadMs(100)            /* fast spread for the test */
    ->setDrainCooldownMs(1000);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* Open two keep-alive sockets — the second accept transitions
     * active_connections to 2 == pause_high and fires the drain. */
    $fp1 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    $fp2 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    stream_set_timeout($fp1, 3);
    stream_set_timeout($fp2, 3);

    /* Issue a request on each. Both responses should carry
     * Connection: close because both were alive when drain fired. */
    fwrite($fp1, "GET /a HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    fwrite($fp2, "GET /b HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");

    /* Give server time to accept + process + spread-jitter up to 100 ms. */
    usleep(250000);

    $r1 = ''; $r2 = '';
    while (!feof($fp1)) { $c = fread($fp1, 4096); if ($c==='' || $c===false) break; $r1 .= $c; if (strpos($r1, "\r\n\r\nok")!==false) break; }
    while (!feof($fp2)) { $c = fread($fp2, 4096); if ($c==='' || $c===false) break; $r2 .= $c; if (strpos($r2, "\r\n\r\nok")!==false) break; }

    fclose($fp1); fclose($fp2);

    echo "r1_has_close=", (int)(stripos($r1, "\r\nConnection: close") !== false), "\n";
    echo "r2_has_close=", (int)(stripos($r2, "\r\nConnection: close") !== false), "\n";

    $tel = $server->getTelemetry();
    /* Don't check listeners_paused — it's transient. Drains caused
     * conn closes → active_connections dropped below pause_low →
     * resume fired before we read telemetry. Check pause_count_total
     * instead (historical — at least one pause occurred). */
    echo "pause_count>=1=",                ($tel['pause_count_total'] >= 1 ? 1 : 0), "\n";
    echo "drain_events_reactive_total=",   (int)$tel['drain_events_reactive_total'], "\n";
    echo "drained_reactive_total>=2=",     ($tel['connections_drained_reactive_total'] >= 2 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
r1_has_close=1
r2_has_close=1
pause_count>=1=1
drain_events_reactive_total=1
drained_reactive_total>=2=1
done
