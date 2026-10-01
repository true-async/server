--TEST--
HttpServer: a reactor-pool worker stopped by its own handler receives no more requests (#346)
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
/* A handler on a pool worker holds that worker's clone of the server, and
 * stop() on it stops the clone alone. The reactors route requests to the
 * inboxes in the worker registry, so the clone's inbox leaves the registry
 * with the stop: a clone still published takes its reactor's share of the
 * requests after the stop and runs them with isRunning() === false. Every
 * request after the stop has to reach the worker that still runs. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-079';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink("$tmp/stderr.txt"); @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)   /* TCP listener required by start() */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) use ($server) {
    if ($req->getPath() === '/stop') {
        $res->setBody('stopping');
        $server->stop();
        return;
    }

    $res->setBody($server->isRunning() ? 'running' : 'STOPPED');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

function h3_get(string $bin, int $port, string $path, string $tmp): string
{
    $cmd = sprintf('H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d %s GET 2>%s',
        escapeshellarg($bin), $port, escapeshellarg($path), escapeshellarg("$tmp/stderr.txt"));
    $body = (string) shell_exec($cmd);
    preg_match('/^STATUS=(\d+)$/m', (string) @file_get_contents("$tmp/stderr.txt"), $m);

    return ($m[1] ?? '-') . ' ' . $body;
}

spawn(function () use ($server, $port, $client_bin, $tmp) {
    /* Reactors + workers need a moment to thread up and bind. */
    delay(600);

    /* Taken before the echo: the stopped worker's exit notice goes to stderr
     * while the request is in flight. */
    $stop = h3_get($client_bin, $port, '/stop', $tmp);
    echo "/stop: $stop\n";

    $answers = [];
    for ($i = 0; $i < 16; $i++) {
        $answer = h3_get($client_bin, $port, '/', $tmp);
        $answers[$answer] = ($answers[$answer] ?? 0) + 1;
    }

    ksort($answers);
    foreach ($answers as $answer => $count) {
        echo "after: $answer x$count\n";
    }

    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECTF--
%A/stop: 200 stopping
%Aafter: 200 running x16
%Adone
