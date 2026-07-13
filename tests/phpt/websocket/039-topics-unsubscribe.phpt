--TEST--
WebSocket topics: unsubscribe(), getTopics(), and a closing connection leaving every topic by itself
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A subscription is thread-local state that a disconnect has to unwind: the
 * session is freed by the connection layer, so ws_session_destroy is what drops
 * it from every topic. An explicit unsubscribe must be idempotent, and a dropped
 * socket must not leave a subscriber behind in the count. */
require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('room');
    $ws->subscribe('room');          // idempotent — still one subscription

    foreach ($ws as $msg) {
        switch ($msg->data) {
            case 'topics':
                $ws->subscribe('extra/#');
                $t = $ws->getTopics();
                sort($t);
                $ws->send('topics:' . implode(',', $t));
                break;

            case 'leave':
                $ws->unsubscribe('room');
                $ws->unsubscribe('room');   // idempotent — a no-op
                $ws->send('ok');
                break;

            case 'count':
                $ws->send('count:' . $ws->subscriberCount('room'));
                break;

            default:
                $ws->publish('room', 'relay:' . $msg->data);
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

function room_count($fp): string {
    ws_write($fp, 'count');
    delay(300);

    return ws_read_pending($fp);
}

spawn(function () use ($port, $server) {
    $a = ws_open($port);
    $b = ws_open($port);
    $c = ws_open($port);

    if ($a === null || $b === null || $c === null) {
        echo "handshake failed\n";
        $server->stop();
        return;
    }

    delay(300);
    echo 'subscribed: ', room_count($c), "\n";

    ws_write($a, 'topics');
    delay(300);
    echo ws_read_pending($a), "\n";

    /* A leaves the topic but stays connected. */
    ws_write($a, 'leave');
    delay(300);
    echo 'after unsubscribe: ', ws_read_pending($a) === 'ok' ? room_count($c) : 'no ack', "\n";

    ws_write($c, 'x');
    delay(300);
    echo 'unsubscribed peer skipped: ', ws_read_pending($a) === '' ? 'yes' : 'no', "\n";

    /* B just drops — nothing calls unsubscribe() for it. */
    fclose($b);
    delay(500);
    echo 'after disconnect: ', room_count($c), "\n";

    fclose($a);
    fclose($c);

    delay(300);
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
subscribed: count:3
topics:extra/#,room
after unsubscribe: count:2
unsubscribed peer skipped: yes
after disconnect: count:1
done%A
