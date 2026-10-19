--TEST--
HTTP/2: a client reset after a refused body still reaches the handler as HttpException 499 (#412)
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
/* The handler catches the refusal (HttpException 413) and goes on writing a
 * streamed response, a chunk every 50 ms. Once the 413 is delivered the client
 * resets the stream with CANCEL, and the handler must hear that as its own
 * second HttpException, code 499 (client gone). Answering the refusal with a
 * status (#412) must not leave the stream in a state where the later cancel is
 * lost or the handler is told the wrong code. The handler codes already read
 * 413,499 before #412; the status line is what the fix changes. */

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
const CANCEL   = 8;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP));

$codes = [];

$server->addHttpHandler(function ($req, $res) use (&$codes) {
    try {
        $req->awaitBody();
    } catch (HttpException $e) {
        $codes[] = $e->getCode();
    }

    try {
        $res->setStatusCode(200);
        for ($i = 0; $i < 100; $i++) {
            $res->write('chunk');
            delay(50);
        }
    } catch (HttpException $e) {
        $codes[] = $e->getCode();
    } catch (\Throwable $e) {
        $codes[] = get_class($e);
    }
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/cancel', "127.0.0.1:$port", [], null, false);
    h2_ack_server_settings($cli);

    delay(100);

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    [$status] = $cli->collectResponse($sid);
    echo "status=$status\n";

    $cli->sendRstStream($sid, CANCEL);
    delay(500);

    $cli->close();
    $server->stop();
});

$server->start();
await($client);
echo "handler codes: ", implode(',', $codes), "\n";
echo "done\n";
--EXPECT--
status=413
handler codes: 413,499
done
