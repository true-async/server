--TEST--
HttpServer: excessive final trailer cancels body waiters and drops pipelined request
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
require_once __DIR__ . '/../_free_port.inc';
$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port)
    ->setBodyStreamingEnabled(true)->setReadTimeout(3)->setWriteTimeout(3));
$entered = 0;
$completed = 0;
$server->addHttpHandler(function ($req, $res) use (&$entered, &$completed) {
    $which = ++$entered;
    if ($which === 1) {
        $req->awaitBody();
    } else {
        while ($req->readBody() !== null) {}
    }
    ++$completed;
    $res->setBody('UNEXPECTED SUCCESS')->end();
});
$client = spawn(function () use ($port, $server, &$entered, &$completed) {
    usleep(20000);
    foreach (['awaitBody', 'readBody'] as $index => $label) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
        stream_set_timeout($fp, 3);
        fwrite($fp, "POST / HTTP/1.1\r\nHost: original\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n");
        // Ensure the handler is parked before completing the invalid message.
        for ($i = 0; $i < 100 && $entered < $index + 1; $i++) usleep(1000);
        fwrite($fp, "0\r\n" . str_repeat("X-Trailer: ignored\r\n", 255)
            . "\r\nGET /must-not-dispatch HTTP/1.1\r\nHost: next\r\n\r\n");
        $wire = stream_get_contents($fp);
        fclose($fp);
        $ok = str_contains($wire, '431') && !str_contains($wire, '200 OK')
            && $completed === 0 && $entered === $index + 1;
        echo "$label: " . ($ok ? 'cancelled' : 'FAILED') . "\n";
    }
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
awaitBody: cancelled
readBody: cancelled
done
