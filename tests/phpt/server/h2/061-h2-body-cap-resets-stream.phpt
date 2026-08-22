--TEST--
HTTP/2: a body past setMaxBodySize is refused with RST_STREAM, not left hanging
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
/* setMaxBodySize is documented to end an over-large HTTP/2 body with a stream
 * reset (src/http_server_config.c, setMaxBodySize). It did not: the check in
 * cb_on_data_chunk_recv answered NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE, and
 * that callback's contract has no such meaning — nghttp2.h documents
 * NGHTTP2_ERR_PAUSE and nothing else, so the refusal was swallowed. Nothing
 * reached the wire, the handler stayed parked in awaitBody(), and the peer
 * waited for a stream that would never be answered or reset.
 *
 * Four 8 KiB frames against a 16 KiB cap: the third one passes the cap while
 * the total is still inside the 65535-byte initial window, so what ends the
 * stream is the cap and not flow control. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP  = 16 * 1024;
const CHUNK     = 8 * 1024;
const CHUNKS    = 4;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP);

$server = new HttpServer($config);

$caught_code = 0;
$caught_msg  = '';
$finished    = false;

$server->addHttpHandler(function ($req, $res) use (&$caught_code, &$caught_msg, &$finished) {
    try {
        $req->awaitBody();
        $finished = true;
        $res->setStatusCode(200)->setBody('unexpected');
    } catch (HttpException $e) {
        $caught_code = $e->getCode();
        $caught_msg  = $e->getMessage();
    }
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    /* No content-length, so the cap can only trip mid-body. */
    $sid = $cli->sendRequest('POST', '/big', "127.0.0.1:$port", [], null, false);

    /* Ack the server SETTINGS, then let the handler reach awaitBody() — the
     * refusal has to find a parked handler, not one still queued. */
    while (true) {
        $fr = $cli->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, , ] = $fr;
        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            break;
        }
    }

    delay(100);

    for ($i = 0; $i < CHUNKS; $i++) {
        $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
    }

    $reset_code = -1;
    while (true) {
        $fr = $cli->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, , $payload] = $fr;

        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
            continue;
        }

        if ($type === H2_FRAME_RST_STREAM) {
            $reset_code = unpack('N', $payload)[1];
            break;
        }
    }

    echo "reset_code=$reset_code\n";
    $cli->close();

    delay(200);
    $server->stop();
});

$server->start();
await($client);

echo "finished=", (int)$finished, "\n";
echo "caught_code=$caught_code\n";
echo "caught_msg=$caught_msg\n";
echo "done\n";
--EXPECT--
reset_code=11
finished=0
caught_code=413
caught_msg=request body exceeds the configured limit
done
