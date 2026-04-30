--TEST--
HttpServer: proactive MAX_CONNECTION_AGE forces Connection: close on next request
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Step 8 — proactive per-connection drain. After the connection has
 * been open for > max_connection_age_ms (± 10% jitter), the next
 * response on that TCP carries `Connection: close`, forcing the
 * client to reopen for its next request (which lets SO_REUSEPORT
 * pick a less loaded worker). Mirrors gRPC MAX_CONNECTION_AGE. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19840 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnectionAgeMs(1000);    /* 1 second */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody('ok');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* Open one keep-alive TCP, send two requests 1.2 s apart. First
     * response should be a normal keep-alive; second crosses the age
     * threshold and must carry Connection: close. */
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    stream_set_timeout($fp, 3);

    $req = "GET /first HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    fwrite($fp, $req);

    /* Read the first response. */
    $r1 = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $r1 .= $c;
        if (strpos($r1, "\r\n\r\nok") !== false) break;
    }
    $first_has_close = (stripos($r1, "\r\nConnection: close") !== false);

    /* Sleep past the 1 s age threshold. */
    usleep(1200000);

    /* Second request — server must mark this conn for drain. */
    fwrite($fp, "GET /second HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    $r2 = '';
    while (!feof($fp)) {
        $c = fread($fp, 4096);
        if ($c === '' || $c === false) break;
        $r2 .= $c;
        if (strpos($r2, "\r\n\r\nok") !== false) break;
    }
    fclose($fp);

    $second_has_close = (stripos($r2, "\r\nConnection: close") !== false);

    echo "first_has_close=", (int)$first_has_close, "\n";
    echo "second_has_close=", (int)$second_has_close, "\n";

    $tel = $server->getTelemetry();
    echo "proactive_total=", (int)$tel['connections_drained_proactive_total'], "\n";
    echo "h1_close_total=",  (int)$tel['h1_connection_close_sent_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
first_has_close=0
second_has_close=1
proactive_total=1
h1_close_total=1
done
