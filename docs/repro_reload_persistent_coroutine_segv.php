<?php
/* Proof: a bootloader that leaves a PERSISTENT async resource on the worker
 * reactor (a never-ending background coroutine with a timer) — mimicking the
 * laravel-spawn async DB pool's healthcheck timer + pooled connections. If
 * reload() deadlocks here while the trivial define()-only bootloader works,
 * the cause is confirmed: long-lived reactor handles block worker retirement. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

$port = (int) ($argv[1] ?? 18200);
$mode = $argv[2] ?? 'persistent';   // 'persistent' or 'trivial'
$dir  = sys_get_temp_dir() . '/tas_persist_' . getmypid();
@mkdir($dir, 0777, true);
$code = $dir . '/version.php';
file_put_contents($code, "<?php return 'v1';");

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5)
    ->setWorkers(2)
    ->setLogSeverity(LogSeverity::INFO)->setLogStream(STDERR)
    ->setBootloader(static function () use ($code, $mode): void {
        define('APP_V', (string) include $code);
        if ($mode === 'persistent') {
            // Persistent background coroutine — arms a recurring timer on the
            // worker reactor that outlives any per-request scope.
            spawn(static function () {
                while (true) {
                    \Async\delay(1000);
                }
            });
        }
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
    for ($i = 0; $i < 50 && $v1 !== 'v1'; $i++) { usleep(200000); $v1 = $curl(); }
    fwrite(STDOUT, "PROOF before=$v1\n");
    file_put_contents($code, "<?php return 'v2';");
    $v2 = '';
    for ($i = 0; $i < 60 && $v2 !== 'v2'; $i++) { usleep(200000); $v2 = $curl(); }
    fwrite(STDOUT, "PROOF after=$v2 (iterations=$i)\n");
    @unlink($code); @rmdir($dir);
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
