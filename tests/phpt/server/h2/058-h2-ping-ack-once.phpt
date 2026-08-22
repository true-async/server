--TEST--
HTTP/2 h2c: one PING gets exactly one ACK, and a closing session still answers
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The server submits the PING ACK itself, because nghttp2's automatic one is
 * guarded by session_is_closing and so never fires on a session the peer has
 * GOAWAYed — which is what a gRPC shutdown sequence looks like. With nghttp2's
 * ACK left on, the two answered a live connection's PING twice.
 *
 * Both halves are asserted here: a live session answers once, and a session the
 * peer has closed still answers. Deleting either the manual submit or the
 * no_auto_ping_ack option fails one of them. */

require_once __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())->addListener('127.0.0.1', $port)->setReadTimeout(10)
);
$server->addHttpHandler(function ($req, $res) {
    $res->setBody('ok')->end();
});

/* Count the ACKs carrying our payload until the stream ends or the peer goes. */
$collect = static function (H2TestClient $c, string $payload): int {
    $acks = 0;

    while (true) {
        $fr = $c->readFrame();
        if ($fr === null) { break; }
        [$type, $flags, $sid, $body] = $fr;

        if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
            $c->sendSettingsAck();
            continue;
        }

        if ($type === H2_FRAME_PING && ($flags & H2_FLAG_ACK) && $body === $payload) {
            $acks++;
            continue;
        }

        if ($type === H2_FRAME_DATA && ($flags & H2_FLAG_END_STREAM)) { break; }
    }

    return $acks;
};

$client = spawn(function () use ($port, $server, $collect) {
    try {
        $live = new H2TestClient('127.0.0.1', $port, 3);
        $live->sendRequest('GET', '/', "127.0.0.1:$port");
        $live->sendPing('LIVEPING');
        echo 'live acks: ', $collect($live, 'LIVEPING'), "\n";
        $live->close();

        /* No request at all, then a peer GOAWAY — the shape a gRPC shutdown
         * arrives in. */
        $closing = new H2TestClient('127.0.0.1', $port, 3);
        $closing->sendRawFrame(H2_FRAME_GOAWAY, 0, 0,
                               pack('N', 0) . pack('N', 0));
        $closing->sendPing('GONEPING');
        echo 'closing acks: ', $collect($closing, 'GONEPING'), "\n";
        $closing->close();
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
live acks: 1
closing acks: 1
done
