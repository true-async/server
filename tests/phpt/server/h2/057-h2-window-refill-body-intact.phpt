--TEST--
HTTP/2 h2c: a client that refills its window every 16 KiB still gets the whole body (issue #219)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The read path used to drain nghttp2 at the socket with send(2), past the
 * writev the emitter had in flight. http2_session_drain takes the bytes out of
 * the session, so whatever the socket refused was gone: the body ended
 * mid-frame and the client waited for the rest.
 *
 * A WINDOW_UPDATE is what puts the read path and the emitter in the same tick,
 * so the refill interval is the knob. At 16 KiB the client wakes the read
 * callback about once per DATA frame; against main this test receives roughly
 * 2.6 MiB of the 3 MiB and never sees END_STREAM.
 *
 * Each chunk carries a distinct byte, so the sha1 catches a reordered or
 * duplicated chunk as well as a lost one. */

require_once __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = tas_free_port();
$CHUNK_SZ = 65536;
$N_CHUNKS = 48;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(30)
    ->setWriteTimeout(30);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($CHUNK_SZ, $N_CHUNKS) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    for ($i = 0; $i < $N_CHUNKS; $i++) {
        $res->write(str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ));
    }

    $res->end();
});

$client = spawn(function () use ($port, $server, $CHUNK_SZ, $N_CHUNKS) {
    usleep(50000);

    try {
        $expected = '';
        for ($i = 0; $i < $N_CHUNKS; $i++) {
            $expected .= str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ);
        }

        $c = new H2TestClient('127.0.0.1', $port, 8);
        $sid = $c->sendRequest('GET', '/big', "127.0.0.1:$port");

        /* Take flow control out of the picture: the socket buffer is what this
         * test wants to be the limit. */
        $c->sendWindowUpdate(0, 0x7000000);
        $c->sendWindowUpdate($sid, 0x7000000);

        $body = '';
        $unacked = 0;
        $ended = false;

        while (true) {
            $fr = $c->readFrame();
            if ($fr === null) { break; }
            [$type, $flags, $fsid, $payload] = $fr;

            if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
                $c->sendSettingsAck();
                continue;
            }

            if ($type !== H2_FRAME_DATA || $fsid !== $sid) { continue; }

            $body .= $payload;
            $unacked += strlen($payload);

            if ($unacked >= 16384) {
                $c->sendWindowUpdate(0, $unacked);
                $c->sendWindowUpdate($sid, $unacked);
                $unacked = 0;
            }

            if ($flags & H2_FLAG_END_STREAM) {
                $ended = true;
                break;
            }
        }

        echo 'end_stream: ', $ended ? 'yes' : 'no', "\n";
        echo 'bytes: ', strlen($body), ' of ', $N_CHUNKS * $CHUNK_SZ, "\n";
        echo 'body: ', sha1($body) === sha1($expected) ? 'intact' : 'CORRUPT', "\n";

        $c->close();
    } catch (Throwable $e) {
        echo 'client error: ', $e->getMessage(), "\n";
    }

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
end_stream: yes
bytes: 3145728 of 3145728
body: intact
done
