--TEST--
HttpResponse::send() — HTTP/2 peer RST mid-stream surfaces as HttpException(499)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* PLAN_STREAMING §9.2 (088-cancel): handler drives a streaming response
 * via repeated send() calls; curl aborts after the first chunk has been
 * delivered. The stream-close callback (cb_on_stream_close in
 * src/http2/http2_session.c) must cancel the handler coroutine with
 * HttpException(499), so the next send() — or any pending write — throws
 * and the handler's try/catch observes the abort.
 *
 * Verifies:
 *  - cancel reaches the handler while it is still suspended between
 *    send() calls,
 *  - exception carries code=499 + the "stream reset by peer" message,
 *  - server state survives (subsequent connection succeeds). */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19860 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$caught_code      = 0;
$caught_msg       = '';
$handler_finished = false;
$chunks_sent      = 0;

$server->addHttpHandler(function ($req, $res)
    use (&$caught_code, &$caught_msg, &$handler_finished, &$chunks_sent) {
    if ($req->getUri() !== '/stream') {
        $res->setStatusCode(200)->setBody('ok');
        return;
    }
    try {
        $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
        /* Stream slowly enough that curl's --max-time kills us between
         * chunks. One send() per iteration, delay in between. */
        for ($i = 0; $i < 20; $i++) {
            $res->send("chunk-$i\n");
            $chunks_sent++;
            delay(100);
        }
        $res->end();
        $handler_finished = true;
    } catch (HttpException $e) {
        $caught_code = $e->getCode();
        $caught_msg  = $e->getMessage();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* Cross-platform replacement for `curl --max-time 0.3` mid-stream
     * abort. curl's --max-time-then-RST behaviour is not portable
     * across cmd.exe / Windows curl combinations (Windows curl returns
     * CURLE_OPERATION_TIMEDOUT in some scenarios but the timing of
     * RST_STREAM emission differs from POSIX). The pure-PHP H2 client
     * gives the test exact frame-level control: open a stream, wait
     * for the first DATA frame from the server (handler is past the
     * first send()), then send RST_STREAM. The server's
     * cb_on_stream_close fires the same code path the curl variant
     * was exercising. */
    $cli = new H2TestClient('127.0.0.1', $port);
    $sid = $cli->sendRequest('GET', '/stream', '127.0.0.1');
    $rst_sent = false;
    while (true) {
        $fr = $cli->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, $sid_in, $payload] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            continue;
        }
        /* First DATA frame on our stream → handler made it past the
         * first send(); RST_STREAM now reaches it suspended between
         * chunks, mirroring the curl --max-time-during-stream window. */
        if ($type === H2_FRAME_DATA && $sid_in === $sid && !$rst_sent) {
            $cli->sendRstStream($sid, /* CANCEL */ 0x08);
            $rst_sent = true;
            break;
        }
    }
    $cli->close();
    echo "rst_sent=", (int)$rst_sent, "\n";

    delay(300);  /* Let the cancel + dispose drive through. */

    /* Liveness probe on a fresh H2 connection — the server must keep
     * accepting new connections after the cancelled one. */
    $probe = new H2TestClient('127.0.0.1', $port);
    $sid2  = $probe->sendRequest('GET', '/ok', '127.0.0.1');
    [$status, $body, , ] = $probe->collectResponse($sid2);
    $probe->close();
    echo "next_req_status=$status body=", trim($body), "\n";

    $server->stop();
});

$server->start();
await($client);

echo "handler_finished=", (int)$handler_finished, "\n";
echo "caught_code=$caught_code\n";
echo "caught_msg=$caught_msg\n";
echo "some_chunks_sent=", (int)($chunks_sent > 0 && $chunks_sent < 20), "\n";
echo "done\n";
?>
--EXPECT--
rst_sent=1
next_req_status=200 body=ok
handler_finished=0
caught_code=499
caught_msg=stream reset by peer
some_chunks_sent=1
done
