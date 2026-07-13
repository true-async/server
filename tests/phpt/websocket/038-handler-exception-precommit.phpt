--TEST--
WebSocket: a handler that throws before any WS I/O answers 5xx and the worker survives (#119)
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

/* Throws before touching $ws — the 101 has not been sent yet, so the upgrade
 * must be refused with a status rather than completed and then closed.
 * Throwable::$code carries the status when it is a valid 4xx/5xx. */
$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    if (str_contains((string)$req->getUri(), 'forbidden')) {
        throw new \RuntimeException('nope', 403);
    }
    throw new \RuntimeException('boom from handler');
});

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(404)->end();
});

/** Try the WS handshake; return the status line the server answered with. */
function ws_try(int $port, string $path): string {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        return "REFUSED";
    }
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET $path HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    $resp = '';
    while (!str_contains($resp, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $resp .= $c;
    }
    fclose($fp);
    return strtok($resp, "\r\n") ?: 'EMPTY';
}

function http_get(int $port): string {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$fp) {
        return "REFUSED";
    }
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $r = fread($fp, 256);
    fclose($fp);
    return strtok((string)$r, "\r\n") ?: 'EMPTY';
}

$client = spawn(function () use ($port, $server) {
    usleep(60000);
    $r = [];
    /* Three throwing upgrades in a row: before the fix the first one killed
     * the worker and the rest were refused. */
    $r[] = ws_try($port, '/boom');
    $r[] = ws_try($port, '/boom');
    $r[] = ws_try($port, '/forbidden');
    $r[] = http_get($port);
    usleep(40000);
    $server->stop();
    return $r;
});

$server->start();
$r = await($client);

echo "ws #1:      ", $r[0], "\n";
echo "ws #2:      ", $r[1], "\n";
echo "ws code=403:", $r[2], "\n";
echo "http after: ", $r[3], "\n";
echo "Done\n";
--EXPECTF--
Warning: [true-async-server] uncaught exception in websocket handler: RuntimeException: boom from handler (in %s:%d) in Unknown on line 0

Warning: [true-async-server] uncaught exception in websocket handler: RuntimeException: boom from handler (in %s:%d) in Unknown on line 0

Warning: [true-async-server] uncaught exception in websocket handler: RuntimeException: nope (in %s:%d) in Unknown on line 0
ws #1:      HTTP/1.1 500 Internal Server Error
ws #2:      HTTP/1.1 500 Internal Server Error
ws code=403:HTTP/1.1 403 Forbidden
http after: HTTP/1.1 404 Not Found
Done
