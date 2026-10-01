--TEST--
HttpServer: a reactor-pool worker stopped by its own handler takes no request while that handler still runs (#346)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* stop() unpublishes the worker's inbox at once, not when start() resumes.
 * The /stop handler keeps its thread for 1 s after stop() without yielding,
 * so start() cannot resume; requests sent in that window that reach the
 * stopping worker's inbox would run after it with isRunning() === false.
 * Every one of them has to reach the worker that still runs, and answer
 * inside the window: an answer after it would prove nothing. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-080';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    foreach (glob("$tmp/stderr-*.txt") as $f) { @unlink($f); }
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) use ($server) {
    if ($req->getPath() === '/stop') {
        $server->stop();

        /* A busy loop, not delay(): the thread must not run start(). */
        $until = hrtime(true) + 1_000_000_000;
        while (hrtime(true) < $until) {
        }

        $res->setBody("stopping $until");
        return;
    }

    /* hrtime() is one monotonic clock for every thread of the process. */
    $res->setBody(($server->isRunning() ? 'running ' : 'STOPPED ') . hrtime(true));
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

function h3_spawn(string $bin, int $port, string $path, string $stderr): array
{
    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d %s GET 2>%s',
        escapeshellarg($bin), $port, escapeshellarg($path), escapeshellarg($stderr));
    $pipes = [];
    $p = proc_open($cmd, [1 => ['pipe', 'w']], $pipes);

    return [$p, $pipes[1], $stderr];
}

function h3_collect(array $client): string
{
    [$p, $out, $stderr] = $client;
    $body = (string) stream_get_contents($out);
    fclose($out);
    proc_close($p);
    preg_match('/^STATUS=(\d+)$/m', (string) @file_get_contents($stderr), $m);

    return ($m[1] ?? '-') . ' ' . $body;
}

spawn(function () use ($server, $port, $client_bin, $tmp) {
    /* Reactors + workers need a moment to thread up and bind. */
    delay(600);

    $stop = h3_spawn($client_bin, $port, '/stop', "$tmp/stderr-stop.txt");

    /* Inside the 1 s the /stop handler holds its thread. */
    delay(250);

    $clients = [];
    for ($i = 0; $i < 16; $i++) {
        $clients[] = h3_spawn($client_bin, $port, '/', "$tmp/stderr-$i.txt");
    }

    /* Taken before the echo: the stopped worker's exit notice goes to stderr
     * while the request is in flight. */
    $stop = h3_collect($stop);
    $until = preg_match('/ stopping (\d+)$/', $stop, $m) ? (int) $m[1] : 0;
    echo "/stop: ", preg_replace('/ \d+$/', '', $stop), "\n";

    $answers = [];
    $inside = 0;
    foreach ($clients as $client) {
        [$status, $state, $at] = explode(' ', h3_collect($client) . '  ', 3);
        $answers["$status $state"] = ($answers["$status $state"] ?? 0) + 1;
        $inside += (int) $at > 0 && (int) $at < $until;
    }

    ksort($answers);
    foreach ($answers as $answer => $count) {
        echo "during: $answer x$count\n";
    }

    echo "answered inside the window: ", $inside > 0 ? 'yes' : 'no', "\n";

    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%A/stop: 200 stopping
%Aduring: 200 running x16
answered inside the window: yes
%Adone
