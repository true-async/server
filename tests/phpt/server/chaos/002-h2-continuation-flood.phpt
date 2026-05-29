--TEST--
Chaos: HTTP/2 CONTINUATION flood (CVE-2024-27316) — server survives + stays live
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
/* CVE-2024-27316 "CONTINUATION flood": a client opens a stream with a
 * HEADERS frame that never sets END_HEADERS, then streams a long run of
 * CONTINUATION frames. A naive server appends each fragment to an
 * unbounded header buffer and is driven to OOM without the stream ever
 * being dispatched. nghttp2 caps the header block; this test pins the
 * invariant that the server SURVIVES the flood (no OOM/hang/crash) and
 * still answers a fresh request afterwards — the connection under attack
 * may be torn down (GOAWAY / close), which is the correct response. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/../h2/_h2_client.inc';

$port = 19872 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addHttp2Handler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

const FLOOD = 2000;     /* CONTINUATION frames, never terminating */

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $c = new H2TestClient('127.0.0.1', $port);
    while (true) {
        $fr = $c->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, , ] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $c->sendSettingsAck();
            break;
        }
    }

    /* HEADERS without END_HEADERS, then FLOOD CONTINUATION frames whose
     * last one also omits END_HEADERS — the header block never closes. */
    try {
        $c->sendHeadersThenContinuations(
            'GET', '/', 'x', FLOOD, /*pad_bytes=*/64,
            /*final_end_headers=*/false, /*end_stream=*/false);
    } catch (\Throwable $e) {
        /* The server may RST/close mid-flood — writes then fail. Expected. */
    }
    $c->close();

    delay(300);

    /* Liveness: a fresh connection must still get a clean 200. */
    $probe = new H2TestClient('127.0.0.1', $port);
    $sid = $probe->sendRequest('GET', '/', 'x');
    [$status, $body, , ] = $probe->collectResponse($sid);
    $probe->close();
    echo "next_req_ok=", ($status === 200 && $body === 'ok' ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
next_req_ok=1
done
