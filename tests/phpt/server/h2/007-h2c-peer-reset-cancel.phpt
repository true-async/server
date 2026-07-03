--TEST--
HttpServer: peer RST_STREAM cancels in-flight HTTP/2 handler with HttpException(499)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif();
?>
--FILE--
<?php
/* Step 7.2a — peer-initiated stream reset must surface in the handler
 * coroutine as HttpException(code=499, "stream reset by peer"), NOT as
 * silent teardown. Verifies that (a) cancel fires with a live coroutine,
 * (b) try/catch{} blocks run, (c) refcount keeps the stream alive long
 * enough for dispose to unwind safely, (d) the server survives and the
 * next connection still works.
 *
 * The reset is sent as a real HTTP/2 RST_STREAM frame over a raw socket
 * rather than relying on `curl --max-time`: curl's abort closes the TCP
 * connection with a FIN on macOS (graceful) vs RST on Linux, so it does
 * not reliably produce a stream-level reset. Hand-built frames do. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);

$caught_code      = 0;
$caught_msg       = '';
$handler_finished = false;

$server->addHttpHandler(function ($req, $res)
    use (&$caught_code, &$caught_msg, &$handler_finished) {
    if ($req->getUri() !== '/slow') {
        /* Fast path for the post-cancel liveness probe. */
        $res->setStatusCode(200)->setBody('ok');
        return;
    }
    try {
        /* Long enough that the RST_STREAM below always races in before
         * natural completion. */
        delay(3000);
        $handler_finished = true;
        $res->setStatusCode(200)->setBody('unexpected');
    } catch (HttpException $e) {
        $caught_code = $e->getCode();
        $caught_msg  = $e->getMessage();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* Open an h2c connection and start the /slow request (END_STREAM so
     * the handler dispatches and parks in delay()). Pump one read to ack
     * the server SETTINGS — readFrame yields to the scheduler, so the
     * server processes our HEADERS and the handler reaches delay(). Then
     * RST_STREAM the live stream. */
    $cli = new H2TestClient('127.0.0.1', $port);
    $sid = $cli->sendRequest('GET', '/slow', 'x');
    while (true) {
        $fr = $cli->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, , ] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            break;
        }
    }
    delay(100);                          /* handler reaches delay(3000) */
    $cli->sendRstStream($sid, 0x8 /* CANCEL */);
    $cli->close();

    /* Give the server a tick to process RST and drive cancel + dispose. */
    delay(200);

    /* Next request on a fresh connection must still work — proves the
     * cancel path didn't damage server state. */
    $probe = new H2TestClient('127.0.0.1', $port);
    $sid2  = $probe->sendRequest('GET', '/ok', 'x');
    [$status, $body, , ] = $probe->collectResponse($sid2);
    $probe->close();
    echo "next_req_ok=", ($status === 200 && $body === 'ok' ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);

echo "handler_finished=", (int)$handler_finished, "\n";
echo "caught_code=$caught_code\n";
echo "caught_msg=$caught_msg\n";
echo "done\n";
--EXPECT--
next_req_ok=1
handler_finished=0
caught_code=499
caught_msg=stream reset by peer
done
