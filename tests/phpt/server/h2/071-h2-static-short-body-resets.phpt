--TEST--
HttpServer: an HTTP/2 static file body cut short resets its stream instead of ending it
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The open-file cache keeps the size it saw for its TTL. Truncating the file
 * after the first request makes the next one declare 2 MiB and find EOF at
 * 100 KiB. An END_STREAM there tells the peer a 100 KiB body is the whole
 * file; RST_STREAM(INTERNAL_ERROR = 2) tells it the body failed. 100 KiB is
 * past the peer's initial 64 KiB window, so the read that finds EOF completes
 * while the ring still holds bytes the window has not let out: the last DATA
 * frame carries no END_STREAM, and the reset is sent after it. The reset is
 * per stream, so the next request on the same connection is answered, and the
 * failure evicted the stale entry, so the file is sized afresh after it. The
 * HTTP/1 counterpart is h1/065. */

require_once __DIR__ . '/_h2_client.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-071';
$root = "$tmp/docroot";
@mkdir($root, 0700, true);

$size = 2 * 1024 * 1024;
$file = "$root/big.bin";

register_shutdown_function(function () use ($tmp, $root, $file) {
    @unlink($file); @unlink("$root/small.txt"); @rmdir($root);
    @rmdir($tmp);
});

file_put_contents($file, str_repeat('ABCDEFGHIJKLMNOP', $size / 16));
file_put_contents("$root/small.txt", 'small');

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);
$server->addStaticHandler((new StaticHandler('/s/', $root))->setOpenFileCache(16, 60));

function h2_get(H2TestClient $cli, int $port, string $path): string
{
    $sid = $cli->sendRequest('GET', $path, "127.0.0.1:$port");
    [$status, $body, , $ended] = $cli->collectResponse($sid, true);

    return "status=$status got=" . strlen($body) . " ended=" . (int) $ended;
}

$client = spawn(function () use ($server, $port, $file) {
    usleep(100000);

    try {
        $cli = new H2TestClient('127.0.0.1', $port, 10);

        /* Fills the cache with the 2 MiB size. */
        echo "warm: ", h2_get($cli, $port, '/s/big.bin'), "\n";

        $fp = fopen($file, 'r+');
        ftruncate($fp, 100 * 1024);
        fclose($fp);
        clearstatcache();

        echo "short: ", h2_get($cli, $port, '/s/big.bin'),
            " reset=", var_export($cli->lastResetCode(), true), "\n";
        echo "next: ", h2_get($cli, $port, '/s/small.txt'), "\n";
        $cli->close();

        /* A fresh client, so a reset here would not be the one read above. */
        $cli = new H2TestClient('127.0.0.1', $port, 10);
        echo "after: ", h2_get($cli, $port, '/s/big.bin'),
            " reset=", var_export($cli->lastResetCode(), true), "\n";
        $cli->close();
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }

    $server->stop();
});

$watchdog = spawn(function () use ($server) {
    usleep(15000000);
    if ($server->isRunning()) { $server->stop(); }
});

$server->start();
$watchdog->cancel();
await($client);
?>
--EXPECT--
warm: status=200 got=2097152 ended=1
short: status=200 got=102400 ended=0 reset=2
next: status=200 got=5 ended=1
after: status=200 got=102400 ended=1 reset=NULL
