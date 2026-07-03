--TEST--
HttpServer: peer RST during awaitBody() wakes handler with HttpException(499)
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
/* Step 7.2a — suspended-on-awaitBody variant of 076. When a client
 * aborts mid-upload, the handler coroutine is parked in the
 * body_event wait; cancel must wake it with HttpException(499)
 * instead of leaving it suspended forever.
 *
 * The abort is a real HTTP/2 RST_STREAM over a raw socket: a POST whose
 * HEADERS carry no END_STREAM leaves the handler parked in awaitBody(),
 * then RST_STREAM resets it. curl's --max-time abort is not portable
 * (FIN on macOS vs RST on Linux), so hand-built frames are used. */

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
    try {
        $req->awaitBody();
        $handler_finished = true;
        $res->setStatusCode(200)->setBody('unexpected');
    } catch (HttpException $e) {
        $caught_code = $e->getCode();
        $caught_msg  = $e->getMessage();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* POST /upload with HEADERS but NO END_STREAM — the body never
     * completes, so the handler parks in awaitBody(). Pump one read to
     * ack the server SETTINGS (yields to the scheduler so the handler
     * dispatches), then RST_STREAM the parked stream. */
    $cli = new H2TestClient('127.0.0.1', $port);
    $sid = $cli->sendRequest('POST', '/upload', 'x', [], null, /*end_stream=*/false);
    while (true) {
        $fr = $cli->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, , ] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            break;
        }
    }
    delay(100);                          /* handler reaches awaitBody() */
    $cli->sendRstStream($sid, 0x8 /* CANCEL */);
    $cli->close();

    /* Let the cancel + dispose unwind. */
    delay(200);

    $server->stop();
});

$server->start();
await($client);

echo "handler_finished=", (int)$handler_finished, "\n";
echo "caught_code=$caught_code\n";
echo "caught_msg=$caught_msg\n";
echo "done\n";
--EXPECT--
handler_finished=0
caught_code=499
caught_msg=stream reset by peer
done
