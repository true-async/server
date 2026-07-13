--TEST--
WebSocket: a handler that throws after the upgrade closes 1011 and the worker survives (#119)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
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
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);

/* /boom commits the upgrade (send) and only then throws: the peer already
 * holds a live session, so the failure has to arrive in-protocol as a
 * CLOSE 1011 (RFC 6455 §7.4.1), not as a silent disconnect. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    if (str_contains((string)$req->getUri(), 'boom')) {
        $ws->send('before-boom');
        throw new \RuntimeException('boom after commit');
    }
    while (($m = $ws->recv()) !== null) {
        $ws->send('echo:' . $m->data);
    }
});

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(404)->end();
});

function ws_client_text_frame(string $payload): string {
    $mask = 'abcd';
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

/** Open a WS connection; return the status line plus every frame that follows. */
function ws_open(int $port, string $path, ?string $send = null): array {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        return ['status' => 'REFUSED', 'frames' => []];
    }
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET $path HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    /* Frames may ride in the same read as the handshake response. */
    $buf = '';
    while (!str_contains($buf, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $buf .= $c;
    }
    $status = strtok($buf, "\r\n") ?: 'EMPTY';
    $pos    = strpos($buf, "\r\n\r\n");
    $tail   = ($pos !== false) ? substr($buf, $pos + 4) : '';

    if ($send !== null) {
        fwrite($fp, ws_client_text_frame($send));
    }

    while (strlen($tail) < 24) {
        $c = fread($fp, 256);
        if ($c === false || $c === '') break;
        $tail .= $c;
        if ($send !== null && strlen($tail) >= 2) break;   /* one echo is enough */
    }
    fclose($fp);

    /* Server→client frames are never masked (RFC 6455 §5.1). */
    $frames = [];
    $i = 0;
    while ($i + 2 <= strlen($tail)) {
        $op  = ord($tail[$i]) & 0x0f;
        $len = ord($tail[$i + 1]) & 0x7f;
        $pay = substr($tail, $i + 2, $len);
        if ($op === 0x8) {
            $code = (strlen($pay) >= 2) ? ((ord($pay[0]) << 8) | ord($pay[1])) : 0;
            $frames[] = "CLOSE($code)";
        } elseif ($op === 0x1) {
            $frames[] = "TEXT($pay)";
        } else {
            $frames[] = "OPCODE($op)";
        }
        $i += 2 + $len;
    }
    return ['status' => $status, 'frames' => $frames];
}

$client = spawn(function () use ($port, $server) {
    usleep(60000);
    $boom = ws_open($port, '/boom');
    usleep(40000);
    /* The worker must still be serving: a normal WS session after the throw. */
    $ok = ws_open($port, '/echo', 'hi');
    usleep(40000);
    $server->stop();
    return ['boom' => $boom, 'ok' => $ok];
});

$server->start();
$r = await($client);

echo "boom status: ", $r['boom']['status'], "\n";
echo "boom frames: ", implode(' ', $r['boom']['frames']), "\n";
echo "next status: ", $r['ok']['status'], "\n";
echo "next frames: ", implode(' ', $r['ok']['frames']), "\n";
echo "Done\n";
--EXPECTF--
Warning: [true-async-server] uncaught exception in websocket handler: RuntimeException: boom after commit (in %s:%d) in Unknown on line 0
boom status: HTTP/1.1 101 Switching Protocols
boom frames: TEXT(before-boom) CLOSE(1011)
next status: HTTP/1.1 101 Switching Protocols
next frames: TEXT(echo:hi)
Done
