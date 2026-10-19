--TEST--
HTTP/2: refused streaming bodies give their connection window credit back (#412)
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
/* The server's connection window is 16 MiB and, with streaming bodies on, the
 * credit for a request byte is granted when the handler pops it from the body
 * queue. A stream refused past setMaxBodySize() leaves its queued bytes unread
 * for good, so each such stream used to eat about 64 KiB of the connection
 * window for the rest of the connection: after ~256 of them the window was
 * empty and a plain request on the same connection met FLOW_CONTROL_ERROR or
 * no answer. The refusal now hands the credit back.
 *
 * Each stream sends 65535 bytes (the stream window) of a body without
 * content-length against a 60 KiB cap; the handler never reads it. 300 of them
 * on one connection exceed 16 MiB, then one small POST must still be answered. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

const BODY_CAP = 60 * 1024;
const CHUNK    = 8 * 1024;
const STREAMS  = 300;
/* Seven full frames and a short one: the stream window, to the byte. */
const LAST_CHUNK = 65535 - 7 * CHUNK;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setBodyStreamingEnabled(true)
    ->setReadTimeout(20)
    ->setWriteTimeout(20)
    ->setMaxBodySize(BODY_CAP));

$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/ok') {
        $res->setStatusCode(200)->setBody('ok');
        return;
    }

    try {
        $req->awaitBody();
    } catch (\Throwable $e) {
    }
});

/* Read until the server resets $sid. False when the connection ends first. */
function wait_for_reset(H2TestClient $cli, int $sid): bool
{
    while (($fr = $cli->readFrame()) !== null) {
        [$type, $flags, $fsid] = $fr;

        if ($type === H2_FRAME_SETTINGS && ($flags & H2_FLAG_ACK) === 0) {
            $cli->sendSettingsAck();
        } elseif ($type === H2_FRAME_GOAWAY) {
            return false;
        } elseif ($type === H2_FRAME_RST_STREAM && $fsid === $sid) {
            return true;
        }
    }

    return false;
}

$client = spawn(function () use ($port, $server) {
    $cli = new H2TestClient('127.0.0.1', $port, 5);
    $refused = 0;

    try {
        for ($n = 0; $n < STREAMS; $n++) {
            $sid = $cli->sendRequest('POST', '/refuse', "127.0.0.1:$port", [], null, false);

            for ($i = 0; $i < 7; $i++) {
                $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', CHUNK));
            }
            $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, str_repeat('a', LAST_CHUNK));

            if (!wait_for_reset($cli, $sid)) {
                break;
            }
            $refused++;
        }

        echo "refused streams: $refused of ", STREAMS, "\n";

        $sid = $cli->sendRequest('POST', '/ok', "127.0.0.1:$port", [], str_repeat('b', 100));
        [$status, $body, , $ended] = $cli->collectResponse($sid);
        echo "last request: status=$status body=$body ended=", (int)$ended, "\n";
    } catch (\RuntimeException $e) {
        echo "refused streams: $refused of ", STREAMS, "\n";
        echo "connection lost: ", $e->getMessage(), "\n";
    }

    $cli->close();
    delay(200);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
refused streams: 300 of 300
last request: status=200 body=ok ended=1
done
