--TEST--
HTTP/2: a body past the cap that arrives with the headers is answered 413, then reset with NO_ERROR (#412)
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
/* HEADERS and four 8 KiB DATA frames leave back to back, with no wait for the
 * server SETTINGS and no pause. The cap (16 KiB) can trip before the handler
 * has started, so there is no handler to answer: the server itself has to send
 * the 413 and then RST_STREAM(NO_ERROR). Before #412 the client got only
 * RST_STREAM and no status. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
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

$server->addHttpHandler(function ($req, $res) {
    $req->awaitBody();
    $res->setStatusCode(200)->setBody('unexpected');
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/big', "127.0.0.1:$port", [], null, false);
    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    [$status, , , $ended] = $cli->collectResponse($sid);
    echo "status=$status\n";
    echo "ended=", (int)$ended, "\n";
    echo "reset_code=", h2_next_reset_code($cli, $sid) ?? 'none', "\n";

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
status=413
ended=1
reset_code=0
done
