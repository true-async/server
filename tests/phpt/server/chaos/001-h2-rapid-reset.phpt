--TEST--
Chaos: HTTP/2 rapid reset (CVE-2023-44487) — burst of open-then-RST streams
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif();
?>
--FILE--
<?php
/* CVE-2023-44487 "Rapid Reset": a client opens a stream and immediately
 * RST_STREAMs it, repeatedly. Each cancel frees a stream slot so the
 * client can open another at near-zero cost while forcing server-side
 * setup/teardown work. This test does not assert a specific mitigation
 * (the server has no hard rate cap yet) — it pins the invariants that
 * matter: the server SURVIVES a 200-stream reset burst, accounts the
 * resets, and still answers a fresh request afterwards (not wedged). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/../h2/_h2_client.inc';

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttp2Handler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

const BURST = 200;

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $c = new H2TestClient('127.0.0.1', $port);
    /* Ack the server SETTINGS so the connection is fully live. */
    while (true) {
        $fr = $c->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, , ] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $c->sendSettingsAck();
            break;
        }
    }

    /* Open-then-RST burst: HEADERS(END_STREAM) immediately followed by
     * RST_STREAM(CANCEL) on the same stream id, BURST times. */
    for ($i = 0; $i < BURST; $i++) {
        $sid = $c->sendRequest('GET', '/', 'x');
        $c->sendRstStream($sid, 0x8 /* CANCEL */);
    }
    $c->close();

    /* Let the server drain the burst. */
    delay(300);

    /* Liveness: a fresh connection must still get a clean 200. */
    $probe = new H2TestClient('127.0.0.1', $port);
    $sid2  = $probe->sendRequest('GET', '/', 'x');
    [$status, $body, , ] = $probe->collectResponse($sid2);
    $probe->close();
    echo "next_req_ok=", ($status === 200 && $body === 'ok' ? 1 : 0), "\n";

    $tel = $server->getTelemetry();
    /* All BURST streams were seen. */
    echo "opened_ge_burst=",
         ($tel['h2_streams_opened_total'] >= BURST ? 1 : 0), "\n";
    /* Nearly all were reset by peer — a couple may complete before their
     * RST lands (the trivial handler answers instantly), so allow slack. */
    echo "most_reset=",
         ($tel['h2_streams_reset_by_peer_total'] >= BURST - 5 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
next_req_ok=1
opened_ge_burst=1
most_reset=1
done
