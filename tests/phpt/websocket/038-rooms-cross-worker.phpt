--TEST--
WebSocket: rooms fan a broadcast across workers, and count() tallies every worker
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A worker is a thread with its own PHP context, so a room cannot be a PHP array
 * of connections: peers of other workers would never be reached. The room lives
 * in the server (ws_hub.c) and a broadcast is handed to each worker, which then
 * writes to its own sockets. Here 6 clients spread over 4 workers: one sends, the
 * other five must receive it whichever worker owns them. */
require_once __DIR__ . '/../server/_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;
use function Async\delay;

const CLIENTS = 6;

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setWorkers(4)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setWsPingIntervalMs(0);

$server = new HttpServer($config);
$room   = $server->room('chat');

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) use ($room) {
    $room->join($ws);

    foreach ($ws as $msg) {
        if ($msg->data === 'count') {
            $ws->send('count:' . $room->count());
            continue;
        }

        $room->broadcast('relay:' . $msg->data, $ws);
    }
});

$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(404)->end(); });

function ws_open(int $port) {
    $fp = false;
    for ($try = 0; $try < 50 && $fp === false; $try++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $e, $s, 2);
        if ($fp === false) { usleep(200000); }
    }
    if ($fp === false) { return null; }

    stream_set_timeout($fp, 3);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') break;
        $hs .= $c;
    }

    return str_contains($hs, '101') ? $fp : null;
}

function ws_write($fp, string $payload): void {
    $mask = random_bytes(4);
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    fwrite($fp, chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked);
}

function ws_read($fp): string {
    $hdr = fread($fp, 2);
    if (strlen($hdr) < 2) { return ''; }

    $len  = ord($hdr[1]) & 0x7f;
    $data = '';
    while (strlen($data) < $len) {
        $chunk = fread($fp, $len - strlen($data));
        if ($chunk === false || $chunk === '') break;
        $data .= $chunk;
    }

    return $data;
}

spawn(function () use ($port, $server) {
    delay(4000);   // four workers have to bind

    $conns = [];
    for ($i = 0; $i < CLIENTS; $i++) {
        $fp = ws_open($port);
        if ($fp === null) { echo "handshake failed\n"; posix_kill(getmypid(), SIGKILL); }
        $conns[] = $fp;
    }

    delay(500);    // let every join land

    /* count() is a scatter/gather over the workers, not a shared counter. */
    ws_write($conns[0], 'count');
    delay(1000);
    stream_set_blocking($conns[0], false);
    $tally = ws_read($conns[0]);

    /* Fan out to everyone but the sender — across all four workers. */
    stream_set_blocking($conns[0], true);
    ws_write($conns[0], 'hello');
    delay(1000);

    $got = 0;
    for ($i = 1; $i < CLIENTS; $i++) {
        stream_set_blocking($conns[$i], false);
        if (ws_read($conns[$i]) === 'relay:hello') { $got++; }
    }

    stream_set_blocking($conns[0], false);
    $echo = ws_read($conns[0]);

    echo 'count across workers: ', $tally === 'count:' . CLIENTS ? 'yes' : "no ($tally)", "\n";
    echo 'all peers received: ',   $got === CLIENTS - 1 ? 'yes' : "no ($got)", "\n";
    echo 'sender excluded: ',      $echo === '' ? 'yes' : 'no', "\n";

    foreach ($conns as $fp) { fclose($fp); }

    /* The pool parent cannot stop() itself (issue #11) — same exit as 034. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
count across workers: yes
all peers received: yes
sender excluded: yes%A
