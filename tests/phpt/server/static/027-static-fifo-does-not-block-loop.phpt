--TEST--
StaticHandler: a FIFO in the mount cannot block the event loop
--EXTENSIONS--
true_async_server
true_async
posix
--SKIPIF--
<?php
if (PHP_OS_FAMILY === 'Windows') die('skip no FIFOs');
exec('curl --version 2>&1', $out, $rc);
if ($rc !== 0) die('skip curl CLI not available');
?>
--FILE--
<?php
/* open(O_RDONLY) on a FIFO without a writer sleeps until one appears, and it
 * sleeps in the event-loop thread. The request for the FIFO goes to PHP under
 * on_missing: Next, and a request for a regular file on another connection is
 * answered meanwhile. A helper opens the FIFO read-write after 3 s, which
 * wakes a blocked open and never blocks itself, so the test ends either way. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use TrueAsync\StaticOnMissing;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
posix_mkfifo("$root/pipe", 0600);
file_put_contents("$root/ok.txt", 'ok');

register_shutdown_function(function () use ($root) {
    @unlink("$root/pipe");
    @unlink("$root/ok.txt");
    @rmdir($root);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));
$server->addStaticHandler((new StaticHandler('/s/', $root))->setOnMissing(StaticOnMissing::NEXT));
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(299)->setBody('php')->end();
});

$run = static function (array $cmd) {
    return proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes) ?: null;
};
$curl = static fn (string $path): array => ['curl', '-s', '-o', '/dev/null', '-w', '%{http_code} %{time_total}',
    '--max-time', '6', "http://127.0.0.1:$port$path"];

$wait = static function ($proc): string {
    while (proc_get_status($proc)['running']) {
        \Async\delay(20);
    }

    return '';
};

$client = spawn(function () use ($run, $curl, $wait, $root, $server) {
    // All three start before the FIFO request can block the loop; the file
    // request leaves 0.3 s later on its own clock, not on the loop's.
    $release = $run(['sh', '-c', 'sleep 3; : 1<>"$0"', "$root/pipe"]);
    $fifo = proc_open($curl('/s/pipe'), [1 => ['pipe', 'w']], $fifo_pipes);
    $file = proc_open(['sh', '-c', 'sleep 0.3; exec "$@"', 'sh', ...$curl('/s/ok.txt')],
        [1 => ['pipe', 'w']], $file_pipes);
    $wait($file);
    [$code, $time] = explode(' ', stream_get_contents($file_pipes[1]));
    echo "ok.txt: $code, ", ((float) $time < 2.0 ? 'before' : 'after'), " the FIFO got a writer\n";

    $wait($fifo);
    echo "pipe: ", explode(' ', stream_get_contents($fifo_pipes[1]))[0], "\n";

    $wait($release);
    $server->stop();
});

$server->start();
await($client);
?>
--EXPECT--
ok.txt: 200, before the FIFO got a writer
pipe: 299
