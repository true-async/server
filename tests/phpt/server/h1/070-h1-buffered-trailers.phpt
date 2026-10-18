--TEST--
HttpServer: buffered request trailers preserve initial headers through body completion
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
    ->setBodyStreamingEnabled(false)->setReadTimeout(3)->setWriteTimeout(3));
$server->addHttpHandler(function ($req, $res) {
    $snapshot = static fn() => [$req->getHeader('host'), $req->getHeader('authorization'),
        $req->getHeader('content-type'), $req->getHeader('x-last'), count($req->getHeaders())];
    $before = $snapshot();
    $body = '';
    while (($part = $req->readBody()) !== null) $body .= $part;
    $req->awaitBody();
    $ok = $before === ['original', 'Bearer original', 'text/plain', null, 5]
        && $snapshot() === $before && $body === 'abc';
    $res->setBody($ok ? 'preserved' : 'CHANGED')->end();
});
$client = spawn(function () use ($port, $server) {
    usleep(20000);
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 2);
    stream_set_timeout($fp, 3);
    fwrite($fp, "POST / HTTP/1.1\r\nHost: original\r\nAuthorization: Bearer original\r\n"
        . "Content-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n");
    usleep(20000);
    fwrite($fp, "0\r\nHost: trailer\r\nAuthorization: Bearer trailer\r\n"
        . "Content-Type: application/json\r\nX-Last: discarded\r\n\r\n");
    $wire = stream_get_contents($fp);
    fclose($fp);
    echo str_contains($wire, '200 OK') && str_contains($wire, 'preserved') ? "preserved\n" : "FAILED\n";
    $server->stop();
});
$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
preserved
done
