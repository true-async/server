--TEST--
HTTP/1: three connections that arrive together, past a cap of two, are all served and none gets a 503
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* setMaxConnections(2): the second accept pauses the listeners while the
 * third connection is already on its way. The paused listener leaves it
 * unaccepted instead of answering it from the hard-cap safety net, and the
 * resume serves it. */

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
    delay(300);
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

    $clients = [];
    for ($i = 0; $i < 3; $i++) {
        $clients[$i] = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
        fwrite($clients[$i], "GET /$i HTTP/1.1\r\nHost: x\r\n\r\n");
        stream_set_timeout($clients[$i], 5);
    }

    foreach ($clients as $i => $fp) {
        echo "client $i: ", read_status($fp), "\n";
        fclose($fp);
    }

    echo "refused at cap: ", $server->getTelemetry()['accepts_refused_at_cap_total'], "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
client 0: HTTP/1.1 200 OK
client 1: HTTP/1.1 200 OK
client 2: HTTP/1.1 200 OK
refused at cap: 0
done
