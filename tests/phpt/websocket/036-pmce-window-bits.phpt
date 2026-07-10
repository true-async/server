--TEST--
WebSocket: permessage-deflate honours server_max_window_bits (RFC 7692 §7.1.2.1)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!extension_loaded('true_async_server')) die('skip');
$cfg = new ReflectionClass(TrueAsync\HttpServerConfig::class);
if (!$cfg->hasMethod('setWsPermessageDeflate')) die('skip built without compression');
?>
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
    ->setWsPermessageDeflate(true);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    /* Touch the API so the 101 commits, then drain to close. */
    while ($ws->recv() !== null) {}
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function ws_upgrade(int $port, string $extensions): string {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    fwrite($fp,
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
      . "Sec-WebSocket-Extensions: $extensions\r\n\r\n");
    stream_set_timeout($fp, 2);
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $chunk = fread($fp, 4096);
        if ($chunk === false || $chunk === '') break;
        $hs .= $chunk;
    }
    fclose($fp);
    return $hs;
}

function ext_line(string $hs): string {
    foreach (explode("\r\n", $hs) as $line) {
        if (stripos($line, 'sec-websocket-extensions:') === 0) {
            return trim(substr($line, strlen('sec-websocket-extensions:')));
        }
    }
    return '(none)';
}

$client = spawn(function () use ($port, $server) {
    usleep(20000);

    /* Pinned window: response must echo the honoured cap. */
    $hs = ws_upgrade($port, 'permessage-deflate; server_max_window_bits=9');
    echo "bits9: ", ext_line($hs), "\n";

    /* Plain offer: full window, no echo needed. */
    $hs = ws_upgrade($port, 'permessage-deflate; client_max_window_bits');
    echo "plain: ", ext_line($hs), "\n";

    /* Unfulfillable window (8 < zlib min 9), no fallback: decline PMCE. */
    $hs = ws_upgrade($port, 'permessage-deflate; server_max_window_bits=8');
    echo "bits8: ", ext_line($hs), "\n";

    usleep(50000);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
--EXPECT--
bits9: permessage-deflate; server_no_context_takeover; client_no_context_takeover; server_max_window_bits=9
plain: permessage-deflate; server_no_context_takeover; client_no_context_takeover
bits8: (none)
Done
