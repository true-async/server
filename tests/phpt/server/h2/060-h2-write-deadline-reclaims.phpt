--TEST--
HTTP/2 h2c: a peer that stops reading is reclaimed by the write deadline, not the read one (issue #223)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A queued write carries no await to time out, and nothing else armed the
 * connection's write deadline for it. So a peer that stopped reading without
 * closing wedged the writev: `out_in_flight` stayed true, the destroy deferred
 * on it, and the deadline sweep re-deferred on every tick — the connection, its
 * io and its read buffer were never reclaimed.
 *
 * The read deadline is set far out here so that only the write one can explain
 * the reclaim: at 30 s the test would time out long before it fired.
 *
 * It guards the deadline's own teardown as well. The timer used to close the io
 * itself, leaving the reactor's handle closed but never disposed — one 520-byte
 * async_io_t per timed-out connection, which run-tests reports as a leak on a
 * debug build. The close belongs to http_connection_destroy, which is the only
 * place that pairs it with the dispose. */

require_once __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(30)
        ->setWriteTimeout(1)
        ->setStatsEnabled(true)
);

$server->addHttpHandler(function ($req, $res) {
    $chunk = str_repeat('x', 65536);

    try {
        for ($i = 0; $i < 128; $i++) {
            $res->write($chunk);
        }

        $res->end();
    } catch (Throwable $e) {
        /* The peer is gone by design; the point of the test is the connection
         * gauge, not what the handler is told. */
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    try {
        $c = new H2TestClient('127.0.0.1', $port, 3);
        $sid = $c->sendRequest('GET', '/big', "127.0.0.1:$port");

        /* Take flow control out of the picture, then stop reading and hold the
         * socket open — the shape a wedged peer has. */
        $c->sendWindowUpdate(0, 0x7000000);
        $c->sendWindowUpdate($sid, 0x7000000);

        $active = -1;

        /* Fifteen seconds against a one-second write deadline: the margin is
         * for a loaded runner, not for the deadline, and the read deadline at
         * 30 s still cannot explain a reclaim inside it. */
        for ($i = 0; $i < 150; $i++) {
            delay(100);
            $active = $server->getStats()['totals']['conns_active_h2'] ?? -1;

            if ($active === 0) {
                break;
            }
        }

        echo 'reclaimed: ', $active === 0 ? 'yes' : "no (active=$active)", "\n";
        $c->close();
    } catch (Throwable $e) {
        echo 'client error: ', $e->getMessage(), "\n";
    }

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
reclaimed: yes
done
