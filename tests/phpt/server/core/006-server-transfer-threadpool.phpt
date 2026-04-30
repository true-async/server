--TEST--
HttpServer: Transfer to ThreadPool workers via transfer_obj
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!PHP_ZTS) die('skip ZTS required');
if (!class_exists('Async\ThreadPool')) die('skip ThreadPool not available');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use Async\ThreadPool;
use function Async\spawn;
use function Async\await;

spawn(function() {
    $config = (new HttpServerConfig('127.0.0.1', 19950))
        ->setBacklog(256)
        ->setReadTimeout(7)
        ->setWriteTimeout(11);

    $server = new HttpServer($config);
    $server->addHttpHandler(function($req, $resp) {
        $resp->setStatusCode(200)->setBody('ok')->end();
    });

    echo "main: ", get_class($server), "\n";
    echo "main: running=", $server->isRunning() ? '1' : '0', "\n";

    $pool = new ThreadPool(2);

    // Worker 1: identity + runtime state after transfer.
    $f1 = $pool->submit(function() use ($server) {
        return sprintf(
            "class=%s running=%d",
            get_class($server),
            (int) $server->isRunning()
        );
    });

    // Worker 2: read frozen config snapshot in the destination thread.
    $f2 = $pool->submit(function() use ($server) {
        $cfg = $server->getConfig();
        $listeners = $cfg->getListeners();
        return sprintf(
            "listeners=%d host=%s port=%d backlog=%d read=%d write=%d locked=%d",
            count($listeners),
            $listeners[0]['host'],
            $listeners[0]['port'],
            $cfg->getBacklog(),
            $cfg->getReadTimeout(),
            $cfg->getWriteTimeout(),
            (int) $cfg->isLocked()
        );
    });

    echo "w1: ", await($f1), "\n";
    echo "w2: ", await($f2), "\n";

    $pool->close();
    echo "done\n";
});
?>
--EXPECT--
main: TrueAsync\HttpServer
main: running=0
w1: class=TrueAsync\HttpServer running=0
w2: listeners=1 host=127.0.0.1 port=19950 backlog=256 read=7 write=11 locked=1
done
