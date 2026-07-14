--TEST--
WebSocket topics over HTTP/2: two WS streams multiplexed on ONE connection, publish excludes the sender's stream only
--SKIPIF--
<?php
if (!TrueAsync\HttpServer::isHttp2()) die('skip built without HTTP/2');
?>
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* H2 multiplexes N WebSocket streams over one TCP connection (RFC 8441), so a
 * session is bound to a STREAM, not to a connection. That is why a publish is
 * addressed by ws_id per session: keyed by connection instead, excludeSelf would
 * silently swallow the message for every other WS stream on the publisher's own
 * TCP — the sibling below.
 *
 * Two streams here, one connection: stream A publishes, stream B must receive,
 * and A must not hear itself. */
require_once __DIR__ . '/../server/h2/_h2_client.inc';
require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(1)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    $ws->subscribe('chat');

    foreach ($ws as $m) {
        $ws->publish('chat', 'relay:' . $m->data);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

function ws_mask_text(string $p): string {
    $mask = random_bytes(4);
    $n = strlen($p);
    $m = '';
    for ($i = 0; $i < $n; $i++) { $m .= chr(ord($p[$i]) ^ ord($mask[$i & 3])); }

    return chr(0x81) . chr(0x80 | $n) . $mask . $m;
}

$client = spawn(function () use ($port) {
    usleep(60000);
    $cli = new H2TestClient('127.0.0.1', $port, 8);

    /* Two Extended CONNECT streams on ONE connection — no END_STREAM, so both
     * stay open in both directions. */
    $a = $cli->sendRequest('CONNECT', '/', 'localhost', [':protocol' => 'websocket'], null, false);
    $b = $cli->sendRequest('CONNECT', '/', 'localhost', [':protocol' => 'websocket'], null, false);

    /* Both handlers must have subscribed before the publish, so drain the
     * accept HEADERS for each stream first. */
    $accepted = [];
    $deadline = microtime(true) + 6;
    while (count($accepted) < 2 && microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$type, $flags, $sid, $payload] = $f;

        if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
            $cli->sendSettingsAck();
            continue;
        }

        if ($type === H2_FRAME_HEADERS && ($sid === $a || $sid === $b)) {
            $accepted[$sid] = true;
        }
    }

    usleep(300000);   // let both subscribes land in the tree

    $cli->sendRawFrame(H2_FRAME_DATA, 0, $a, ws_mask_text('h2'));

    /* Collect WS DATA per stream until the sibling has spoken. */
    $got = [$a => [], $b => []];
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$type, $flags, $sid, $payload] = $f;

        if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
            $cli->sendSettingsAck();
            continue;
        }

        if ($type === H2_FRAME_DATA && isset($got[$sid]) && strlen($payload) >= 2) {
            $len = ord($payload[1]) & 0x7f;
            $got[$sid][] = substr($payload, 2, $len);

            if (count($got[$b]) > 0) { break; }
        }
    }

    /* Give a stray frame for A a chance to show up before declaring it silent. */
    usleep(200000);
    $f = $cli->readFrame();
    if ($f !== null && $f[0] === H2_FRAME_DATA && $f[2] === $a && strlen($f[3]) >= 2) {
        $got[$a][] = substr($f[3], 2, ord($f[3][1]) & 0x7f);
    }

    $cli->sendRstStream($a, 0);
    $cli->sendRstStream($b, 0);
    $cli->close();

    return ['accepted' => count($accepted), 'a' => $got[$a], 'b' => $got[$b]];
});

$stopper = spawn(function () use ($server, $client) {
    await($client);
    usleep(40000);
    if ($server->isRunning()) { $server->stop(); }
});

spawn(function () use ($server) {       // safety net
    usleep(9000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$r = await($client);
await($stopper);

echo 'both streams accepted: ', $r['accepted'] === 2 ? 'yes' : 'no', "\n";
echo 'sibling stream on the SAME connection received: ',
     $r['b'] === ['relay:h2'] ? 'yes' : 'no (' . implode(',', $r['b']) . ')', "\n";
echo 'publisher stream excluded: ', $r['a'] === [] ? 'yes' : 'no (' . implode(',', $r['a']) . ')', "\n";
echo "Done\n";
?>
--EXPECT--
both streams accepted: yes
sibling stream on the SAME connection received: yes
publisher stream excluded: yes
Done
