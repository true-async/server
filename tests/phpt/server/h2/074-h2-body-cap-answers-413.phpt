--TEST--
HTTP/2: a body past setMaxBodySize is answered with 413 before the stream is reset (#412)
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
/* On HTTP/1 a body over the cap gets 413. On HTTP/2 the same refusal sent only
 * RST_STREAM, so the client saw a stream error and no status. RFC 9113 §8.1
 * lets a server answer before the request body is complete and then reset the
 * stream with NO_ERROR; the client reads the status.
 *
 * The handler waits in awaitBody() and writes nothing, so no response headers
 * have left when the cap trips: the server is still free to answer. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP = 16 * 1024;
const CHUNK    = 8 * 1024;
const CHUNKS   = 4;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(BODY_CAP);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) {
    $req->awaitBody();
    $res->setStatusCode(200)->setBody('unexpected');
});

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);

    $sid = $cli->sendRequest('POST', '/big', "127.0.0.1:$port", [], null, false);

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

    [$status, , , $ended] = $cli->collectResponse($sid);

    echo "status=$status\n";
    echo "ended=", (int)$ended, "\n";

    /* After the response the upload is cut with NO_ERROR, not an error code. */
    $reset_code = $cli->lastResetCode();
    if ($ended && $reset_code === null) {
        while (($fr = $cli->readFrame()) !== null) {
            [$type, , $fsid, $payload] = $fr;
            if ($type === H2_FRAME_RST_STREAM && $fsid === $sid) {
                $reset_code = unpack('N', $payload)[1];
                break;
            }
        }
    }
    echo "reset_code=", $reset_code ?? 'none', "\n";

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
