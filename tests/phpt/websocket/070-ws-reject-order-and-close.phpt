--TEST--
WebSocket: a refused upgrade answers in pipeline order and closes at once (issue #221)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The 4xx that refuses an upgrade used to be written straight at the io, past
 * the response still waiting in the connection's coalesce tail — so a client
 * that pipelined three requests read the 426 as the answer to the second.
 * Nothing then asked the connection to go either: the 4xx says
 * `Connection: close` and the socket was held to the read deadline.
 *
 * Both refusal paths are covered. The first connection is refused inside
 * ws_dispatch_try_upgrade (version 8 is not 13); the second reaches the handler
 * and is refused by WebSocketUpgrade::reject(). */

require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpRequest;
use TrueAsync\WebSocket;
use TrueAsync\WebSocketUpgrade;
use function Async\spawn;
use function Async\await;

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req, WebSocketUpgrade $u) {
    $u->reject(401);
});
$server->addHttpHandler(function ($req, $res) {
    $res->setBody('S' . ltrim($req->getPath(), '/'))->end();
});

/* Three requests in one write, the third an upgrade. Returns the statuses, the
 * bodies, and how long the peer waited for EOF after the last byte. */
$pipeline = static function (int $port, string $version): array {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($fp, 4);

    fwrite($fp,
        "GET /1 HTTP/1.1\r\nHost: x\r\n\r\n"
      . "GET /2 HTTP/1.1\r\nHost: x\r\n\r\n"
      . "GET /3 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      . "Sec-WebSocket-Version: $version\r\n\r\n");

    $started = microtime(true);
    $wire = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 65536);

        if ($chunk === false || $chunk === '') { break; }

        $wire .= $chunk;
    }

    $waited = microtime(true) - $started;
    fclose($fp);

    preg_match_all('/HTTP\/1\.1 (\d{3})/', $wire, $codes);
    preg_match_all('/\r\n\r\n(S\d)/', $wire, $bodies);

    return [implode(',', $codes[1]), implode(',', $bodies[1]), $waited];
};

$client = spawn(function () use ($port, $server, $pipeline) {
    usleep(50000);

    try {
        foreach (['8' => 426, '13' => 401] as $version => $expected) {
            [$codes, $bodies, $waited] = $pipeline($port, (string)$version);

            echo "version $version: $codes | $bodies | ",
                 $waited < 1.0 ? 'closed at once' : sprintf('HELD %.2fs', $waited), "\n";
        }
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
version 8: 200,200,426 | S1,S2 | closed at once
version 13: 200,200,401 | S1,S2 | closed at once
done
