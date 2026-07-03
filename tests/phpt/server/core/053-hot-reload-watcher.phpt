--TEST--
HttpServerConfig::enableHotReload — file change triggers a pool rotation, new code served (#93)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
if (!exec('curl --version 2>/dev/null')) die('skip curl CLI not available');
?>
--FILE--
<?php
/* The bootloader includes a watched PHP file for the app version. Rewriting
 * that file must — with no manual reload() call — rotate the pool via the
 * built-in watcher trigger and serve the new value on the same port. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/../_free_port.inc';
$port = tas_free_port();

$dir  = sys_get_temp_dir() . '/tas_hotreload_' . getmypid();
@mkdir($dir, 0777, true);
$code = $dir . '/version.php';
file_put_contents($code, "<?php return 'v1';");

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setWorkers(2)
    ->setBootloader(static function () use ($code): void {
        define('APP_V', (string) include $code);
    })
    ->enableHotReload([$dir], ['php'], 150, 1000);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody(APP_V);
});

spawn(function () use ($port, $code, $dir) {
    $curl = static fn (): string => (string) shell_exec(sprintf(
        'curl -s --max-time 2 http://127.0.0.1:%d/ 2>/dev/null', $port));

    $v1 = '';
    for ($i = 0; $i < 50 && $v1 !== 'v1'; $i++) {
        usleep(200000);
        $v1 = $curl();
    }

    echo "before=", $v1, "\n";

    /* The only action: rewrite the watched file. */
    file_put_contents($code, "<?php return 'v2';");

    $v2 = '';
    for ($i = 0; $i < 100 && $v2 !== 'v2'; $i++) {
        usleep(200000);
        $v2 = $curl();
    }

    echo "after=", $v2, "\n";

    @unlink($code);
    @rmdir($dir);
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
before=v1
%A
after=v2%A
