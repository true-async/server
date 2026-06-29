--TEST--
WebSocket: HTTP/2 Extended CONNECT (RFC 8441) — accept + echo over a stream
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
require_once __DIR__ . '/../server/h2/_h2_client.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;

$port = 19890 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($m = $ws->recv()) !== null) {
        $ws->send('echo:' . $m->data);
    }
});
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

/** Masked client→server WebSocket text frame (RFC 6455 §5.2/§5.3). */
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

    /* Extended CONNECT: :method=CONNECT + :protocol=websocket, no END_STREAM
     * so the stream stays open both ways. */
    $sid = $cli->sendRequest('CONNECT', '/', 'localhost',
                             [':protocol' => 'websocket'], null, false);

    /* Send one WS text frame inside a DATA frame. */
    $cli->sendRawFrame(H2_FRAME_DATA, 0, $sid, ws_mask_text('hi-h2'));

    $got_headers = false;
    $echo = null;
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$type, $flags, $fsid, $payload] = $f;
        if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
            $cli->sendSettingsAck();
            continue;
        }
        if ($type === H2_FRAME_HEADERS && $fsid === $sid) {
            $got_headers = true;
        }
        if ($type === H2_FRAME_DATA && $fsid === $sid && strlen($payload) >= 2) {
            $op  = ord($payload[0]) & 0x0f;
            $len = ord($payload[1]) & 0x7f;
            $echo = ['op' => $op, 'data' => substr($payload, 2, $len)];
            break;
        }
    }
    $cli->sendRstStream($sid, 0);
    $cli->close();
    return ['headers' => $got_headers, 'echo' => $echo];
});

$stopper = spawn(function () use ($server, $client) {
    await($client);
    usleep(40000);
    if ($server->isRunning()) { $server->stop(); }
});

spawn(function () use ($server) {       // safety net
    usleep(4000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$r = await($client);
await($stopper);

echo "accept (200 HEADERS): ", ($r['headers'] ? 'yes' : 'no'), "\n";
echo "echo opcode: 0x", ($r['echo'] ? dechex($r['echo']['op']) : '?'), "\n";
echo "echo payload: ", ($r['echo']['data'] ?? '<none>'), "\n";
echo "Done\n";
--EXPECT--
accept (200 HEADERS): yes
echo opcode: 0x1
echo payload: echo:hi-h2
Done
