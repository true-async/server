--TEST--
HTTP/1: a connection that arrives while the listeners are paused waits in the backlog and is served after the resume
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* setMaxConnections(2) pauses the listeners at the second accept. A third
 * client that connects during the pause must stay in the kernel backlog: the
 * server does not accept it, so it gets no 503 from the hard-cap safety net,
 * and once the two parked handlers answer and their connections close, the
 * resume accepts it and it gets its 200. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnections(2)
    ->setDrainSpreadMs(100)
    ->setDrainCooldownMs(1000);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    if ($req->getUri() !== '/third') {
        delay(600);
    }
    $res->setStatusCode(200)->setBody('ok');
});

function read_status($fp): string
{
    $buf = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) {
            break;
        }
        $buf .= $c;
        if (strpos($buf, "\r\n\r\n") !== false) {
            break;
        }
    }
    return strtok($buf, "\r\n") ?: '(nothing)';
}

$client = spawn(function () use ($port, $server) {
    delay(30);

    $fp1 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    $fp2 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    fwrite($fp1, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    fwrite($fp2, "GET /b HTTP/1.1\r\nHost: x\r\n\r\n");
    delay(100);

    echo "paused=", (int)$server->getTelemetry()['listeners_paused'], "\n";

    $fp3 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    fwrite($fp3, "GET /third HTTP/1.1\r\nHost: x\r\n\r\n");
    stream_set_timeout($fp3, 5);

    foreach ([$fp1, $fp2] as $fp) {
        stream_set_timeout($fp, 5);
        echo "first two: ", read_status($fp), "\n";
    }
    fclose($fp1);
    fclose($fp2);

    echo "third: ", read_status($fp3), "\n";
    fclose($fp3);

    echo "refused at cap: ", $server->getTelemetry()['accepts_refused_at_cap_total'], "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
paused=1
first two: HTTP/1.1 200 OK
first two: HTTP/1.1 200 OK
third: HTTP/1.1 200 OK
refused at cap: 0
done
