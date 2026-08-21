--TEST--
Pipelined HTTP/1 responses come back in request order whatever their size
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A pipelined client has no request id to match on: it reads responses in the
 * order they arrive (RFC 9112 §9.3.1). The dispose picks its sender by body
 * size, and the two differ in more than the syscall — one queues behind the
 * write already in flight, the other used to submit past it, so a large
 * response overtook a small one still waiting in the coalesce tail and the
 * client read the wrong body for two of its three requests.
 *
 * The sizes straddle HTTP_WRITEV_THRESHOLD deliberately: two under it and one
 * over, sent as one write so all three land in the read buffer together. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    $n = (int) ltrim($req->getPath(), '/');
    $res->setBody($n >= 3 ? str_repeat("L$n", 900) : "S$n")->end();
});

spawn(function () use ($port, $server) {
    usleep(50000);

    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 3);
    stream_set_timeout($c, 3);

    fwrite($c,
        "GET /1 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /2 HTTP/1.1\r\nHost: x\r\n\r\n" .
        "GET /3 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    $all = '';
    while (!feof($c)) {
        $read = fread($c, 65536);

        if ($read === false || $read === '') {
            break;
        }

        $all .= $read;
    }

    fclose($c);

    preg_match_all('/\r\n\r\n(S\d|L\d)/', $all, $m);
    echo 'order: ', implode(',', $m[1]), "\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
order: S1,S2,L3
Done
