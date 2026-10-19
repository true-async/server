--TEST--
HTTP/2: a body past the cap after the handler already answered ends with RST_STREAM(NO_ERROR) (#412)
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
/* The handler answers 403 at once without reading the body, and the client
 * reads that whole response before it sends any DATA. The response is decided
 * and delivered, so the refusal that follows must not turn it into an error:
 * the stream ends with RST_STREAM(NO_ERROR), not ENHANCE_YOUR_CALM (code 11,
 * what it sent before #412). The status is 403 because H2TestClient does not
 * decode Huffman-coded status values, which 401 is. */

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
    $res->setStatusCode(403)->setBody('denied');
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/early', "127.0.0.1:$port", [], null, false);
    h2_ack_server_settings($cli);

    /* The whole response arrives while the request body is still open. */
    [$status, $body, , $ended] = $cli->collectResponse($sid);
    echo "status=$status\n";
    echo "body=$body\n";
    echo "ended=", (int)$ended, "\n";

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    echo "reset_code=", h2_next_reset_code($cli, $sid) ?? 'none', "\n";

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
status=403
body=denied
ended=1
reset_code=0
done
