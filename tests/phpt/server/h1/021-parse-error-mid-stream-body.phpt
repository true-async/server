--TEST--
HttpServer: chunked body exceeding limit cancels in-flight handler with 413
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19821 + getmypid() % 1000;
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10);
$server = new HttpServer($config);

// Handler waits for the body. The mid-stream limit hit should cancel
// us via HttpException(413) before awaitBody returns naturally.
$handlerRanBody = false;
$server->addHttpHandler(function ($req, $res) use (&$handlerRanBody) {
    $req->awaitBody();
    $handlerRanBody = true;
    // If we ever get here, the test failed (limit didn't fire).
    $res->setStatusCode(200)->setBody('handler ran past awaitBody')->end();
});

$client = spawn(function () use ($port, $server, &$handlerRanBody) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 4);

    // Headers: chunked transfer-encoding so the size isn't pre-checked.
    $headers = "POST / HTTP/1.1\r\n"
             . "Host: x\r\n"
             . "Transfer-Encoding: chunked\r\n"
             . "\r\n";
    fwrite($fp, $headers);

    // Send chunks until we exceed default 10 MB max_body_size or
    // the server tears down the socket. 1 MB chunks: 11 should suffice.
    $chunk = str_repeat('A', 1024 * 1024);
    $chunkLen = sprintf("%x\r\n", strlen($chunk));
    for ($i = 0; $i < 12; $i++) {
        $w = @fwrite($fp, $chunkLen . $chunk . "\r\n");
        if ($w === false || $w === 0) break;  // server closed
    }

    stream_set_timeout($fp, 3);
    $r = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $r .= $c;
    }
    fclose($fp);

    $lines = explode("\r\n", $r);
    echo "status: " . ($lines[0] ?? '(empty)') . "\n";
    echo "handler_ran_past_awaitBody: " . ($handlerRanBody ? 'yes' : 'no') . "\n";

    $tel = $server->getTelemetry();
    echo "parse_errors_413_total=" . $tel['parse_errors_413_total'] . "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECTF--
status: HTTP/1.1 413%a
handler_ran_past_awaitBody: no
parse_errors_413_total=1
done
