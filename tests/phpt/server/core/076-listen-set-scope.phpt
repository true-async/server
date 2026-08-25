--TEST--
HttpServer: two independent servers on one port still conflict (#275)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
?>
--FILE--
<?php
/*
 * The listen set is scoped to one server object, which is what keeps the
 * lazy bind honest: threads of the same server share a socket, while two
 * unrelated servers asking for the same address must still collide.
 *
 * A set shared process-wide would make the second start() succeed here and
 * silently steal half the traffic of the first, which is precisely the
 * failure mode SO_REUSEADDR carries on Windows.
 */
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$first  = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port));
$second = new HttpServer((new HttpServerConfig())->addListener('127.0.0.1', $port));

foreach ([$first, $second] as $server) {
    $server->addHttpHandler(function ($req, $res) {
        $res->setStatusCode(200)->setBody('ok');
    });
}

$client = spawn(function () use ($port, $first, $second) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    try {
        $second->start();
        echo "second: bound the same port\n";
        $second->stop();
    } catch (\Throwable $t) {
        echo "second: refused\n";
    }

    $first->stop();
});

$first->start();
await($client);

echo "Done\n";
?>
--EXPECT--
second: refused
Done
