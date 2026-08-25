--TEST--
A streaming handler that throws resets its HTTP/2 stream and leaves the connection alone
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* HTTP/2 can say "this body failed" without losing anything else, and that is
 * what makes the reset the right answer here: RST_STREAM(INTERNAL_ERROR = 2)
 * on the one stream, END_STREAM on none of them.
 *
 * Two assertions do the work. The reset code is read off the frame rather than
 * inferred from a short body — a stream that merely stopped and a stream that
 * was reset look the same by length, which is the whole defect. And a second
 * request is sent on the same connection afterwards and must complete
 * normally: a connection-level GOAWAY would satisfy the first assertion and
 * fail every other stream in flight.
 *
 * A third assertion is the body: `body=alphabeta` — what the session can frame
 * within the peer's window of everything the handler wrote before the throw,
 * with the reset saying the body stops there. The append path can leave frames
 * queued inside the session, its emit skipped while a writev is in flight and
 * left to the write completion's re-drive, so the abort flushes the session
 * before it submits the reset. Without that flush the reset moves nghttp2's
 * stream to its closing state, where the send predicate refuses DATA and the
 * queued HEADERS go with it: the peer reads status=0 on a response the handler
 * wrote in full. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(15)
        ->setWriteTimeout(15)
);

$server->addHttpHandler(function ($req, $res) {
    if ($req->getUri() === '/ok') {
        $res->end('fine');
        return;
    }

    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
    $res->write('alpha');
    $res->write('beta');

    if ($req->getUri() === '/named') {
        /* ENHANCE_YOUR_CALM — a code this server would never pick itself. */
        $res->abort(0x0b);
        return;
    }

    throw new \RuntimeException('handler failed mid-body');
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    try {
        $cli = new H2TestClient('127.0.0.1', $port, 15);

        $sid = $cli->sendRequest('GET', '/stream', "127.0.0.1:$port");
        [$status, $body, , $ended] = $cli->collectResponse($sid, true);

        echo "status=$status\n";
        echo "body=", $body, "\n";
        echo "ended=", (int) $ended, "\n";
        echo "reset=", var_export($cli->lastResetCode(), true), "\n";

        /* A code the handler named reaches the frame unchanged. */
        $sid3 = $cli->sendRequest('GET', '/named', "127.0.0.1:$port");
        $cli->collectResponse($sid3, true);
        echo "named reset=", var_export($cli->lastResetCode(), true), "\n";

        /* Same connection: the reset was per-stream, not per-connection. */
        $sid2 = $cli->sendRequest('GET', '/ok', "127.0.0.1:$port");
        [$status2, $body2, , $ended2] = $cli->collectResponse($sid2, true);
        $cli->close();

        echo "next=$status2/$body2/", (int) $ended2, "\n";
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
status=200
body=alphabeta
ended=0
reset=2
named reset=11
next=200/fine/1
done
