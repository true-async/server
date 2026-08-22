--TEST--
HTTP/2 h2c: one PING gets exactly one ACK
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The server submits the PING ACK itself, because nghttp2 withholds its
 * automatic one from a closing session (session_is_closing) and a gRPC shutdown
 * sequence waits on that ACK. With nghttp2's ACK left on as well, one PING was
 * answered twice.
 *
 * Only the live session is asserted. A closing one is not reachable from here:
 * a peer GOAWAY on an idle connection winds the session down inside the same
 * feed, so whether a PING behind it is read at all depends on which TCP segment
 * carries it — that is what made this test fail once on macOS. Measured before
 * the fix, that shape answered twice too, so it never stood in the state it was
 * written for. */

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
done
