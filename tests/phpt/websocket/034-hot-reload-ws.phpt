--TEST--
WebSocket: pool reload with a live WS connection — old conn retired, fresh worker serves WS (#93)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
?>
--FILE--
<?php
/* A WS echo connection is open when HttpServer::reload() rotates the pool.
 * The old worker's drain cancels the perpetual WS handler after the (short)
 * grace window, the connection dies, the rotation completes, and a NEW WS
 * connection to the same port echoes against the fresh cohort. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\WebSocket;
use TrueAsync\HttpRequest;
use function Async\spawn;

require __DIR__ . '/../server/_free_port.inc';
$port = tas_free_port();

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setShutdownTimeout(1)   /* keep the drain grace short for the rotation */
    ->setWorkers(2);

$server = new HttpServer($config);

$server->addWebSocketHandler(function (WebSocket $ws, HttpRequest $req) {
    while (($msg = $ws->recv()) !== null) {
        $ws->send('echo: ' . $msg->data);
    }
});

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(404)->end();
});

function ws_client_text_frame(string $payload): string {
    $mask = "abcd";
    $masked = '';
    for ($i = 0, $n = strlen($payload); $i < $n; $i++) {
        $masked .= chr(ord($payload[$i]) ^ ord($mask[$i & 3]));
    }
    return chr(0x81) . chr(0x80 | strlen($payload)) . $mask . $masked;
}

function read_server_frame($fp): array {
    $hdr = '';
    while (strlen($hdr) < 2) {
        $c = fread($fp, 2 - strlen($hdr));
        if ($c === false || $c === '') return ['opcode' => -1, 'data' => ''];
        $hdr .= $c;
    }
    $opcode = ord($hdr[0]) & 0x0f;
    $len    = ord($hdr[1]) & 0x7f;
    if ($len === 126) {
        $extra = fread($fp, 2);
        $len = (ord($extra[0]) << 8) | ord($extra[1]);
    }
    $data = '';
    while (strlen($data) < $len) {
        $c = fread($fp, $len - strlen($data));
        if ($c === false || $c === '') break;
        $data .= $c;
    }
    return ['opcode' => $opcode, 'data' => $data];
}

function ws_connect(int $port) {
    $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if ($fp === false) return false;
    stream_set_timeout($fp, 5);
    fwrite($fp,
      "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    $hs = '';
    while (!str_contains($hs, "\r\n\r\n")) {
        $c = fread($fp, 4096);
        if ($c === false || $c === '') return false;
        $hs .= $c;
    }
    return str_contains($hs, ' 101 ') ? $fp : false;
}

spawn(function () use ($server, $port) {
    usleep(400000);   /* let the pool come up */

    /* Live WS connection on the old cohort. */
    $fp = false;
    for ($i = 0; $i < 25 && $fp === false; $i++) {
        usleep(200000);
        $fp = ws_connect($port);
    }

    if ($fp === false) {
        echo "pre_ws=fail\n";
        posix_kill(getmypid(), SIGKILL);
    }

    fwrite($fp, ws_client_text_frame('one'));
    $r1 = read_server_frame($fp);
    echo "pre_echo=", $r1['data'], "\n";

    /* Rotate while the connection is open. */
    $ok = $server->reload();
    echo "reload=", var_export($ok, true), "\n";

    /* The retired worker must end our connection: close frame (8) or EOF (-1). */
    $r2 = read_server_frame($fp);
    echo "old_conn_ended=", (int)($r2['opcode'] === 8 || $r2['opcode'] === -1), "\n";
    fclose($fp);

    /* Fresh cohort serves new WS connections on the same port. */
    $fp2 = false;
    for ($i = 0; $i < 50 && $fp2 === false; $i++) {
        usleep(200000);
        $fp2 = ws_connect($port);
    }

    if ($fp2 !== false) {
        fwrite($fp2, ws_client_text_frame('two'));
        $r3 = read_server_frame($fp2);
        echo "post_echo=", $r3['data'], "\n";
        fclose($fp2);
    } else {
        echo "post_echo=fail\n";
    }

    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
pre_echo=echo: one
%A
reload=true
old_conn_ended=1
post_echo=echo: two%A
