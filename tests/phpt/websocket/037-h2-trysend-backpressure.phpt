--TEST--
WebSocket: trySend() over HTTP/2 reports BUSY instead of silently dropping frames
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Regression: the H2 chunk ring is slot- AND byte-bounded, and the
 * non-suspending sink discards a frame it cannot take. Before the transport
 * `sendable` gate, trySend() queued into that sink and still returned true —
 * so a handler looping on trySend() lost every frame past the ring with no
 * signal at all (accepted 100, delivered 8). Mirrors 023 for H1. */
require_once __DIR__ . '/../server/h2/_h2_client.inc';
require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

const MSGS     = 100;
const MSG_SIZE = 2048;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0)            // isolate: no server PING traffic
    ->setStreamWriteBufferBytes(4096);  // small high-water mark → backpressure

$server = new HttpServer($config);

$outcome = ['accepted' => 0, 'busy' => 0, 'completed' => false];

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use (&$outcome) {
    $payload = str_repeat('x', MSG_SIZE);
    for ($i = 0; $i < MSGS; $i++) {
        if ($ws->trySend($payload)) {
            $outcome['accepted']++;
        } else {
            $outcome['busy']++;
        }
    }
    $outcome['completed'] = true;   // trySend must never suspend
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

$client = spawn(function () use ($port, $server, &$outcome) {
    usleep(60000);
    $cli = new H2TestClient('127.0.0.1', $port, 8);

    /* Pin SETTINGS_INITIAL_WINDOW_SIZE (0x4) small: the server can only put a
     * sliver on the wire, so the rest has to sit in the stream's chunk ring. */
    $cli->sendRawFrame(H2_FRAME_SETTINGS, 0, 0, pack('nN', 0x4, 1024));

    $sid = $cli->sendRequest('CONNECT', '/', 'localhost',
                             [':protocol' => 'websocket'], null, false);

    /* Empty masked WS frame — commits the upgrade and starts the handler. */
    $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, "\x81\x80\x00\x00\x00\x00");

    /* Never read while the handler runs, so the ring stays full. */
    delay(400);

    /* Open the window and drain whatever the server accepted. */
    $cli->sendWindowUpdate(0, 8 << 20);
    $cli->sendWindowUpdate($sid, 8 << 20);

    $data = '';
    $deadline = microtime(true) + 3;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$type, $flags, $fsid, $payload] = $f;
        if ($type === H2_FRAME_DATA && $fsid === $sid) {
            $data .= $payload;
        }
    }

    /* Count server→client text frames (unmasked, 2048 bytes → 4-byte header). */
    $delivered = 0;
    $off = 0;
    $n = strlen($data);
    while ($off + 4 <= $n) {
        $len = unpack('n', substr($data, $off + 2, 2))[1];
        if ($len !== MSG_SIZE) break;
        $off += 4 + $len;
        $delivered++;
    }

    echo 'accepted>0: ', $outcome['accepted'] > 0 ? 'yes' : 'no', "\n";
    echo 'busy>0: ',     $outcome['busy'] > 0     ? 'yes' : 'no', "\n";
    echo 'completed: ',  $outcome['completed']    ? 'yes' : 'no', "\n";
    /* The point of the test: everything trySend() accepted actually shipped. */
    echo 'no silent loss: ', $delivered === $outcome['accepted'] ? 'yes' : 'no', "\n";

    $cli->close();
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
accepted>0: yes
busy>0: yes
completed: yes
no silent loss: yes
Done
