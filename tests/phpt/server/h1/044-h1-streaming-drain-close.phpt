--TEST--
HttpServer: a streaming response carries the drain's Connection: close
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A drain retires a connection by telling the client not to reuse it, and the
 * telling has to happen inside the header block. A streaming response commits
 * that block at its first write(), while the drain decision used to be taken
 * after the handler returned — so every streamed answer went out without the
 * header and the client kept pooling a socket the server was closing.
 *
 * Two keep-alive sockets reach the hard cap, which fires the reactive drain;
 * the same handler streams on both. The counters are read as well as the
 * header, because the decision now lives in the streaming path and must still
 * be counted once per connection. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

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
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    $res->write('ok');
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $fp1 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    $fp2 = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    stream_set_timeout($fp1, 3);
    stream_set_timeout($fp2, 3);

    fwrite($fp1, "GET /a HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    fwrite($fp2, "GET /b HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");

    usleep(250000);

    $read = static function ($fp) {
        $all = '';

        while (!feof($fp)) {
            $c = fread($fp, 4096);

            if ($c === '' || $c === false) {
                break;
            }

            $all .= $c;

            if (str_contains($all, "0\r\n\r\n")) {
                break;
            }
        }

        return $all;
    };

    $r1 = $read($fp1);
    $r2 = $read($fp2);

    fclose($fp1);
    fclose($fp2);

    echo "r1 close: ", (int) (stripos($r1, "\r\nconnection: close") !== false), "\n";
    echo "r2 close: ", (int) (stripos($r2, "\r\nconnection: close") !== false), "\n";
    echo "r1 chunked: ", (int) (stripos($r1, "\r\nTransfer-Encoding: chunked") !== false), "\n";
    echo "r1 body: ", json_encode(substr($r1, strpos($r1, "\r\n\r\n") + 4)), "\n";

    $tel = $server->getTelemetry();
    echo "drained>=2: ", ($tel['connections_drained_reactive_total'] >= 2 ? 1 : 0), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
r1 close: 1
r2 close: 1
r1 chunked: 1
r1 body: "2\r\nok\r\n0\r\n\r\n"
drained>=2: 1
done
