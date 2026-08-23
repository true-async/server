--TEST--
HTTP/2: a peer that shuts its write half down still gets the whole response (#249)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The half-close reaches the session as a read EOF, and the response is still
 * inside nghttp2 at that moment. A latch on that EOF stopped the re-drive, so
 * the peer read the first batch and then nothing — no END_STREAM, no way to
 * tell a finished body from a cut one.
 *
 * The window has to be granted before the shutdown: a client that has closed
 * its write half can no longer send WINDOW_UPDATE. */

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

const CHUNK_BYTES = 64 * 1024;
const CHUNK_COUNT = 16;
const WINDOW = 8 * 1024 * 1024;

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(5)
);
$server->addHttpHandler(function ($req, $res) {
    $res->setHeader('content-type', 'text/plain');

    for ($i = 0; $i < CHUNK_COUNT; $i++) {
        $res->write(str_repeat('z', CHUNK_BYTES));
    }

    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($fp, 5);
    stream_set_write_buffer($fp, 0);
    stream_set_read_buffer($fp, 0);

    $frame = function (int $type, int $flags, int $sid, string $payload) {
        return chr((strlen($payload) >> 16) & 0xff)
             . chr((strlen($payload) >> 8) & 0xff)
             . chr(strlen($payload) & 0xff)
             . chr($type) . chr($flags) . pack('N', $sid) . $payload;
    };

    fwrite($fp, H2_PREFACE);
    /* SETTINGS_INITIAL_WINDOW_SIZE for the stream, WINDOW_UPDATE for the
     * connection: both are needed to let a 1 MiB body through in one go. */
    fwrite($fp, $frame(H2_FRAME_SETTINGS, 0, 0, pack('n', 0x4) . pack('N', WINDOW)));
    fwrite($fp, $frame(H2_FRAME_WINDOW_UPDATE, 0, 0, pack('N', WINDOW)));

    $hdrs = H2TestClient::encodeRequestHeaders('GET', '/', '127.0.0.1');
    fwrite($fp, $frame(H2_FRAME_HEADERS,
                       H2_FLAG_END_HEADERS | H2_FLAG_END_STREAM, 1, $hdrs));
    fflush($fp);

    stream_socket_shutdown($fp, STREAM_SHUT_WR);

    $body = 0;
    $ended = false;

    while (true) {
        $hdr = '';

        while (strlen($hdr) < 9) {
            $part = fread($fp, 9 - strlen($hdr));

            if ($part === false || $part === '') { break 2; }

            $hdr .= $part;
        }

        $len = (ord($hdr[0]) << 16) | (ord($hdr[1]) << 8) | ord($hdr[2]);
        $type = ord($hdr[3]);
        $flags = ord($hdr[4]);
        $payload = '';

        while (strlen($payload) < $len) {
            $part = fread($fp, $len - strlen($payload));

            if ($part === false || $part === '') { break 2; }

            $payload .= $part;
        }

        if ($type === H2_FRAME_DATA) {
            $body += strlen($payload);

            if ($flags & H2_FLAG_END_STREAM) { $ended = true; }
        }

        if ($type === H2_FRAME_GOAWAY) { break; }
    }

    fclose($fp);

    echo 'body bytes: ', $body, ' of ', CHUNK_BYTES * CHUNK_COUNT, "\n";
    echo 'END_STREAM: ', $ended ? 'yes' : 'no', "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
body bytes: 1048576 of 1048576
END_STREAM: yes
done
