--TEST--
WebSocket H1: data frames keep their order while auto-PONGs are emitted through the other connection writer
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* On HTTP/1 the connection has two writers. A producer send() goes through
 * http_connection_send → send_raw, which submits a uv_write and awaits it.
 * An internal send — the auto-PONG for an inbound PING — goes through
 * http_connection_send_batched, which parks bytes in conn->out_pending_buf
 * when a write is already in flight and flushes them from the completion
 * callback.
 *
 * Both drain the same wslay byte stream, so if a parked tail were overtaken
 * by a later direct submit, the stream would desynchronise exactly as chunked
 * framing does. This test floods the server with PINGs while it pushes a
 * numbered burst the client is not reading, then checks that every data frame
 * arrived, in order, and that the stream parsed at all. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../server/_free_port.inc';

const N_MSG    = 120;
const PAYLOAD  = 4096;
const N_PINGS  = 200;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);   // only the client's PINGs drive the internal path

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->recv();                      // wait for "go"

    $pad = str_repeat('.', PAYLOAD);

    for ($i = 0; $i < N_MSG; $i++) {
        $ws->send($i . '|' . $pad);
    }

    $ws->recv();                      // hold the connection open for the reader
});

$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

function ws_client_frame(int $opcode, string $payload): string {
    $mask   = random_bytes(4);
    $masked = '';

    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }

    $len = strlen($payload);

    if ($len < 126) {
        $head = chr(0x80 | $opcode) . chr(0x80 | $len);
    } else {
        $head = chr(0x80 | $opcode) . chr(0x80 | 126) . pack('n', $len);
    }

    return $head . $mask . $masked;
}

/** Read exactly $n bytes or return null. */
function read_n($fp, int $n): ?string {
    $buf = '';

    while (strlen($buf) < $n) {
        $c = fread($fp, $n - strlen($buf));

        if ($c === '' || $c === false) {
            return null;
        }

        $buf .= $c;
    }

    return $buf;
}

/** Read one server frame (unmasked); [opcode, payload] or null at EOF. */
function read_frame($fp): ?array {
    $hdr = read_n($fp, 2);

    if ($hdr === null) {
        return null;
    }

    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;

    if ($len === 126) {
        $ext = read_n($fp, 2);
        if ($ext === null) return null;
        $len = unpack('n', $ext)[1];
    } elseif ($len === 127) {
        $ext = read_n($fp, 8);
        if ($ext === null) return null;
        $len = unpack('J', $ext)[1];
    }

    $data = $len > 0 ? read_n($fp, $len) : '';

    if ($data === null) {
        return null;
    }

    return [$opcode, $data];
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($fp, 5);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $hs .= $c;
    }

    /* Start the burst, then flood control frames without reading: the socket
     * buffer fills, writes stop completing inline, and the two writers are
     * live at the same time. */
    fwrite($fp, ws_client_frame(0x1, 'go'));

    /* Spaced, not batched: each PING arrives in its own read callback, so
     * each auto-PONG is its own flush through the internal writer. A flush
     * that lands while the previous one is still in flight is the one that
     * parks bytes in the pending tail — the state a later direct submit
     * could overtake. */
    for ($i = 0; $i < N_PINGS; $i++) {
        fwrite($fp, ws_client_frame(0x9, 'p' . $i));
        usleep(1500);
    }

    usleep(200000);

    $seen    = [];
    $pongs   = 0;
    $garbled = 0;

    while (count($seen) < N_MSG) {
        $frame = read_frame($fp);

        if ($frame === null) {
            break;
        }

        [$opcode, $payload] = $frame;

        if ($opcode === 0xa) {
            $pongs++;
            continue;
        }

        if ($opcode !== 0x1) {
            continue;
        }

        $sep = strpos($payload, '|');

        if ($sep === false || strlen($payload) !== $sep + 1 + PAYLOAD) {
            $garbled++;
            continue;
        }

        $seen[] = (int) substr($payload, 0, $sep);
    }

    fclose($fp);
    usleep(20000);
    $server->stop();

    $expected = range(0, N_MSG - 1);

    return [count($seen), $seen === $expected ? 1 : 0, $garbled, $pongs > 0 ? 1 : 0];
});

$server->start();
[$count, $inOrder, $garbled, $sawPong] = await($client);

echo "messages: $count of ", N_MSG, "\n";
echo "in order: $inOrder\n";
echo "garbled: $garbled\n";
echo "saw pong: $sawPong\n";
echo "Done\n";
?>
--EXPECT--
messages: 120 of 120
in order: 1
garbled: 0
saw pong: 1
Done
