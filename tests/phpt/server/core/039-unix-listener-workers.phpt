--TEST--
HttpServer: AF_UNIX listener with workers > 1 — shared listen fd across the pool
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip POSIX-only test (AF_UNIX shared fd)');
if (!extension_loaded('posix')) die('skip posix extension not available');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* AF_UNIX has no SO_REUSEPORT, so the workers>1 fan-out cannot have each
 * worker bind the same path. The pool parent binds the socket once and
 * shares the fd; every worker thread adopts a dup and accepts on it.
 * Before the shared-fd path, worker #2 died on bind() with EADDRINUSE. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;

$path = sys_get_temp_dir() . '/ta-unix-workers.sock';
$root = sys_get_temp_dir() . '/ta-uxw-' . getmypid() . '-' . bin2hex(random_bytes(4));
@unlink($path);
mkdir($root, 0700, true);
file_put_contents("$root/hello.txt", "served-over-unix");

$config = (new HttpServerConfig())
    ->addUnixListener($path)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2);

$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/static/', $root))->disableIndex()
);

spawn(function () use ($path, $root, $server) {
    usleep(400000);  // let the pool thread up and adopt the shared fd

    $hits = 0;
    for ($i = 0; $i < 16; $i++) {
        $out = (string) shell_exec(sprintf(
            'curl -s --max-time 2 --unix-socket %s http://localhost/static/hello.txt',
            escapeshellarg($path)));
        if ($out === 'served-over-unix') {
            $hits++;
        }
    }
    echo "hits=", ($hits === 16 ? 'all' : (string) $hits), "\n";
    echo "done\n";

    @unlink("$root/hello.txt");
    @rmdir($root);
    @unlink($path);

    $server->stop();
});

$server->start();
?>
--CLEAN--
<?php
@unlink(sys_get_temp_dir() . '/ta-unix-workers.sock');
?>
--EXPECTF--
hits=all
done
%A
