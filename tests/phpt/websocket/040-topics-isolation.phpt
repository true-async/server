--TEST--
WebSocket topics: a publish stays inside its topic; publishBinary; excludeSelf=false
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A single-topic test passes even when matching is completely broken, so this one
 * runs two topics at once: a publish into one must not surface in the other.
 * Also covers the binary fan-out (its own opcode + deflate branch) and the
 * publisher opting back in to its own message. */
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
    $topic = $req->getPath() === '/blue' ? 'blue' : 'red';
    $ws->subscribe($topic);

    foreach ($ws as $msg) {
        switch ($msg->data) {
            case 'bin':
                $ws->publishBinary($topic, "\x00\x01\x02");
                break;

            case 'self':
                $ws->publish($topic, 'echo', false);   // excludeSelf = false
                break;

            default:
                $ws->publish($topic, 'relay:' . $msg->data);
        }
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

spawn(function () use ($port, $server) {
    $r1 = ws_open($port, '/red');
    $r2 = ws_open($port, '/red');
    $b1 = ws_open($port, '/blue');

    if ($r1 === null || $r2 === null || $b1 === null) {
        echo "handshake failed\n";
        $server->stop();
        return;
    }

    delay(300);

    ws_write($r1, 'hi');
    delay(300);
    echo 'peer in same topic: ',   ws_read_pending($r2), "\n";
    echo 'other topic untouched: ', ws_read_pending($b1) === '' ? 'yes' : 'no', "\n";

    ws_write($r1, 'bin');
    delay(300);
    stream_set_blocking($r2, false);
    $frame = ws_read_frame($r2);
    stream_set_blocking($r2, true);
    echo 'binary opcode: ', $frame['opcode'] ?? 0, ', bytes: ',
        bin2hex($frame['data'] ?? ''), "\n";

    ws_write($r1, 'self');
    delay(300);
    echo 'excludeSelf=false reaches publisher: ',
        ws_read_pending($r1) === 'echo' ? 'yes' : 'no', "\n";

    fclose($r1);
    fclose($r2);
    fclose($b1);

    delay(300);
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
peer in same topic: relay:hi
other topic untouched: yes
binary opcode: 2, bytes: 000102
excludeSelf=false reaches publisher: yes
done%A
