--TEST--
HttpResponse::sendFile() refuses a path outside open_basedir (#280)
--EXTENSIONS--
true_async_server
true_async
--INI--
open_basedir={PWD}
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
?>
--FILE--
<?php
/*
 * open_basedir is the operator's boundary, and sendFile() opens its file
 * without going through PHP streams — so nothing else would enforce it.
 *
 * The trust sendFile() places in handler code is a contract with the
 * application; it must not let the application out of the box the operator
 * put it in, since the handler is exactly what open_basedir contains.
 *
 * The target is the running interpreter's own binary: certainly readable by
 * the process, certainly outside the boundary, and present on every platform
 * without the test having to create it — creating one is impossible here,
 * because the boundary is already in force when the script starts.
 *
 * No _free_port.inc for the same reason: requiring it would cross the
 * boundary. A fixed high port is enough for a single-listener test.
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$outside = PHP_BINARY;
$port    = 18201;

echo "fopen: ", @fopen($outside, 'rb') ? "allowed\n" : "refused\n";

$server = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port));
$server->addHttpHandler(function ($req, $res) use ($outside) {
    $res->sendFile($outside);
});

$client = spawn(function () use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $probe = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($probe) { fclose($probe); break; }
        usleep(20000);
    }

    $sock = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    if (!$sock) {
        echo "no connection\n";
        $server->stop();
        return;
    }

    fwrite($sock, "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $resp = '';
    while (!feof($sock)) {
        $chunk = fread($sock, 4096);
        if ($chunk === '' || $chunk === false) break;
        $resp .= $chunk;
    }
    fclose($sock);

    echo "status: ", str_contains($resp, ' 500 ') ? "500\n" : "not 500\n";
    echo "binary leaked: ", str_contains($resp, "MZ") || str_contains($resp, "\x7fELF") ? "yes\n" : "no\n";
    $server->stop();
});

$server->start();
await($client);

echo "Done\n";
?>
--EXPECT--
fopen: refused
status: 500
binary leaked: no
Done
