--TEST--
WebSocket: foreach ($ws as $msg) iterates messages like a recv() loop
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
use function Async\delay;

require_once __DIR__ . '/../server/_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5);
$server = new HttpServer($config);

// Traversable via the internal iterator — no PHP-level Iterator methods.
$is_traversable = (new ReflectionClass(WebSocket::class))->implementsInterface(Traversable::class);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    foreach ($ws as $i => $msg) {              // <- iterator drives recv()
        $ws->send("echo:$i:" . $msg->data);
    }
    // foreach ends when the peer closes gracefully.
});
$server->addHttpHandler(function ($req, $resp) { $resp->setStatusCode(404)->end(); });

function ws_text(string $p): string {
    $mask = "wxyz"; $m = '';
    for ($i = 0, $n = strlen($p); $i < $n; $i++) $m .= chr(ord($p[$i]) ^ ord($mask[$i & 3]));
    return chr(0x81) . chr(0x80 | strlen($p)) . $mask . $m;
}
function read_frame($fp): array {
    $h = ''; while (strlen($h) < 2) { $c = fread($fp, 2 - strlen($h)); if ($c === '' || $c === false) return [-1, '']; $h .= $c; }
    $op = ord($h[0]) & 0x0f; $len = ord($h[1]) & 0x7f; $d = '';
    while (strlen($d) < $len) { $c = fread($fp, $len - strlen($d)); if ($c === '' || $c === false) break; $d .= $c; }
    return [$op, $d];
}

$client = spawn(function () use ($port, $server) {
    delay(20);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = ''; while (!str_contains($hs, "\r\n\r\n")) { $c = fread($fp, 4096); if ($c === '' || $c === false) break; $hs .= $c; }

    fwrite($fp, ws_text("hi"));  [$o1, $d1] = read_frame($fp);
    fwrite($fp, ws_text("yo"));  [$o2, $d2] = read_frame($fp);

    // graceful close
    fwrite($fp, chr(0x88) . chr(0x82) . "wxyz" . (function(){ $b="\x03\xe8"; $m="wxyz"; $r=''; for($i=0;$i<2;$i++)$r.=chr(ord($b[$i])^ord($m[$i&3])); return $r; })());
    fclose($fp);
    delay(50);
    $server->stop();
    return ['d1' => $d1, 'd2' => $d2];
});

$server->start();
$r = await($client);

echo "traversable=", $is_traversable ? 'yes' : 'no', "\n";
echo "msg1=", $r['d1'], "\n";
echo "msg2=", $r['d2'], "\n";
echo "Done\n";
--EXPECT--
traversable=yes
msg1=echo:0:hi
msg2=echo:1:yo
Done
