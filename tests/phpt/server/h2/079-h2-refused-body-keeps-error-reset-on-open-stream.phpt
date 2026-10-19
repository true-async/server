--TEST--
HTTP/2: a refused body resets a streamed response the handler is still writing with an error code (#412 guard)
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
/* Guard for #412: this test passes without the fix and has to keep passing.
 * The handler has committed a streamed response (write() sent the headers and
 * a chunk) and then waits for the request body. Its response is not decided
 * yet: it is still being written, so the server cannot deliver it whole. The
 * refusal ends the stream with ENHANCE_YOUR_CALM (code 11), as before, so the
 * client sees a broken stream and not a response that looks complete. The
 * handler still gets HttpException 413. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require __DIR__ . '/_h2_refusal.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP = 16 * 1024;
const CHUNK    = 8 * 1024;
const CHUNKS   = 4;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP));

$caught = 'none';

$server->addHttpHandler(function ($req, $res) use (&$caught) {
    $res->setStatusCode(200)->write('first');

    try {
        $req->awaitBody();
    } catch (HttpException $e) {
        $caught = $e->getCode();
    }
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/stream', "127.0.0.1:$port", [], null, false);
    h2_ack_server_settings($cli);

    delay(100);

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    [$status, $body, , $ended] = $cli->collectResponse($sid);
    echo "status=$status\n";
    echo "body=$body\n";
    echo "ended=", (int)$ended, "\n";
    echo "reset_code=", h2_next_reset_code($cli, $sid) ?? 'none', "\n";

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "handler caught: $caught\n";
echo "done\n";
--EXPECT--
status=200
body=first
ended=0
reset_code=11
handler caught: 413
done
