--TEST--
HttpServer: two independent servers on one port still conflict (#275)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
/* Where the kernel load-balances SO_REUSEPORT, two unrelated servers on one
 * port is a supported arrangement rather than a conflict, and each binds its
 * own socket without consulting any set. The property under test exists only
 * in the sharing camp — which the second Linux run enters through the env. */
if ((PHP_OS_FAMILY === 'Linux' || PHP_OS_FAMILY === 'BSD')
    && getenv('TRUE_ASYNC_SERVER_SHARED_LISTEN_FD') !== '1') {
    die('skip kernel load-balanced SO_REUSEPORT: independent binds are legal here');
}
?>
--FILE--
<?php
/*
 * The listen set is scoped to one server object, which is what keeps the
 * lazy bind honest: threads of the same server share a socket, while two
 * unrelated servers asking for the same address must still collide.
 *
 * A set shared process-wide would make the second start() succeed here and
 * silently take half the traffic of the first, which is the failure mode
 * SO_REUSEADDR carries on Windows.
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

    /* start() blocks for as long as the server runs, so a bind that wrongly
     * succeeds would hang the test rather than fail it. This watchdog turns
     * that outcome back into a readable one. */
    $watchdog = spawn(function () use ($second) {
        usleep(1000000);
        $second->stop();
    });

    try {
        $second->start();
        echo "second: bound the same port\n";
    } catch (\Throwable $t) {
        echo "second: refused\n";
        $second->stop();
    }

    await($watchdog);
    $first->stop();
});

$first->start();
await($client);

echo "Done\n";
?>
--EXPECT--
second: refused
Done
