--TEST--
WebSocket over HTTP/2 (RFC 8441): a throwing handler closes 1011, the connection keeps serving (#119)
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

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

/* The Extended CONNECT path spawns its own handler coroutine (ws_h2_*), so it
 * needs the same failure contract as the H1 upgrade: consume the exception,
 * report it in-protocol, keep the worker. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    if (str_contains((string)$req->getUri(), 'boom')) {
        $ws->send('before-boom');
        throw new \RuntimeException('boom over h2');
    }
    while (($m = $ws->recv()) !== null) {
        $ws->send('echo:' . $m->data);
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

/** Decode the WS frames carried by one DATA payload. */
function ws_decode(string $payload): array {
    $out = [];
    $i = 0;
    while ($i + 2 <= strlen($payload)) {
        $op  = ord($payload[$i]) & 0x0f;
        $len = ord($payload[$i + 1]) & 0x7f;
        $pay = substr($payload, $i + 2, $len);
        if ($op === 0x8) {
            $code = (strlen($pay) >= 2) ? ((ord($pay[0]) << 8) | ord($pay[1])) : 0;
            $out[] = "CLOSE($code)";
        } elseif ($op === 0x1) {
            $out[] = "TEXT($pay)";
        } else {
            $out[] = "OPCODE($op)";
        }
        $i += 2 + $len;
    }
    return $out;
}

$client = spawn(function () use ($port) {
    usleep(60000);
    $cli = new H2TestClient('127.0.0.1', $port, 8);

    /* Stream A: the handler sends one frame, then throws. */
    $boom_sid = $cli->sendRequest('CONNECT', '/boom', 'localhost',
                                  [':protocol' => 'websocket'], null, false);

    $boom_status = null;
    $boom_frames = [];
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$type, $flags, $sid, $payload] = $f;
        if ($type === H2_FRAME_SETTINGS && !($flags & H2_FLAG_ACK)) {
            $cli->sendSettingsAck();
            continue;
        }
        if ($type === H2_FRAME_HEADERS && $sid === $boom_sid) {
            $boom_status = 'HEADERS';
        }
        if ($type === H2_FRAME_DATA && $sid === $boom_sid) {
            $boom_frames = array_merge($boom_frames, ws_decode($payload));
        }
        if (in_array('CLOSE(1011)', $boom_frames, true)) {
            break;
        }
    }

    /* Stream B on the SAME connection: proves the throw took down neither the
     * H2 connection nor the worker. */
    $ok_sid = $cli->sendRequest('CONNECT', '/echo', 'localhost',
                                [':protocol' => 'websocket'], null, false);
    $cli->sendRawFrame(H2_FRAME_DATA, 0, $ok_sid, ws_mask_text('hi-h2'));

    $ok_frames = [];
    $deadline = microtime(true) + 6;
    while (microtime(true) < $deadline) {
        $f = $cli->readFrame();
        if ($f === null) break;
        [$type, $flags, $sid, $payload] = $f;
        if ($type === H2_FRAME_DATA && $sid === $ok_sid && $payload !== '') {
            $ok_frames = ws_decode($payload);
            break;
        }
    }

    $cli->sendRstStream($ok_sid, 0);
    $cli->close();
    return ['boom_status' => $boom_status, 'boom' => $boom_frames, 'ok' => $ok_frames];
});

$stopper = spawn(function () use ($server, $client) {
    await($client);
    usleep(40000);
    if ($server->isRunning()) { $server->stop(); }
});

spawn(function () use ($server) {       // safety net
    usleep(8000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$r = await($client);
await($stopper);

echo "boom accepted: ", ($r['boom_status'] === 'HEADERS' ? 'yes' : 'no'), "\n";
echo "boom frames:   ", implode(' ', $r['boom']), "\n";
echo "next frames:   ", implode(' ', $r['ok']), "\n";
echo "Done\n";
--EXPECTF--
Warning: [true-async-server] uncaught exception in websocket handler: RuntimeException: boom over h2 (in %s:%d) in Unknown on line 0
boom accepted: yes
boom frames:   TEXT(before-boom) CLOSE(1011)
next frames:   TEXT(echo:hi-h2)
Done
