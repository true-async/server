--TEST--
WebSocket: frames pipelined behind the upgrade past the read buffer all reach the handler
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The upgrade GET keeps the connection's request in flight until the handler's
 * first WebSocket I/O commits it, and frames the client sent right behind the
 * GET wait in the 8 KiB read buffer meanwhile. More than that stops the read;
 * the commit empties the buffer into the WebSocket parser and must start the
 * read again, or the rest of the frames stay in the socket (#341). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';
require_once __DIR__ . '/_ws_client.inc';

const FRAMES = 100;
const PAYLOAD = 120;

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0));

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    /* No WebSocket I/O yet: the upgrade stays in flight while the frames land. */
    delay(300);

    $messages = 0;
    $bytes = 0;

    while ($messages < FRAMES && ($m = $ws->recv()) !== null) {
        $messages++;
        $bytes += strlen($m->data);
    }

    $ws->send("got $messages messages, $bytes bytes");
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function masked_frame(string $payload): string
{
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }

    return chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

$client = spawn(function () use ($port, $server) {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 5);

    $wire = "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    for ($i = 0; $i < FRAMES; $i++) {
        $wire .= masked_frame(str_pad("m$i", PAYLOAD, '.'));
    }

    echo "pipelined past the buffer: ", strlen($wire) > 8192 ? 'yes' : 'no', "\n";
    fwrite($fp, $wire);

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $hs .= $chunk;
    }

    echo "upgraded: ", str_contains($hs, ' 101 ') ? 'yes' : 'no', "\n";
    ws_pushback($fp, substr($hs, strpos($hs, "\r\n\r\n") + 4));
    echo ws_await($fp), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
pipelined past the buffer: yes
upgraded: yes
got 100 messages, 12000 bytes
done
