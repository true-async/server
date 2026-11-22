--TEST--
TLS: a peer that resets or closes the connection cancels a handler suspended after its first chunk with HttpException 499
--EXTENSIONS--
true_async_server
true_async
sockets
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/* h1/077 over TLS. While a handler runs, the connection has to keep reading, or
 * neither the FIN nor the RST of a client that left is ever seen. */
require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp_dir = __DIR__ . '/tmp-021';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert = $tmp_dir . '/cert.pem';
$key  = $tmp_dir . '/key.pem';
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed\n";
    exit(1);
}

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port, true)
        ->setCertificate($cert)
        ->setPrivateKey($key)
        ->setReadTimeout(30)
        ->setWriteTimeout(30)
);

$done = 0;

$server->addHttpHandler(function ($req, $res) use ($server, &$done) {
    $path = $req->getPath();
    $started = hrtime(true);

    try {
        $res->write("first\n");
        delay(20000);
        echo "$path: woke up\n";
    } catch (HttpException $e) {
        echo "$path: caught HttpException ", $e->getCode(), "\n";
    } finally {
        $ms = (hrtime(true) - $started) / 1e6;
        echo "$path: finally ", $ms < 5000 ? 'in time' : "after {$ms} ms", "\n";

        if (++$done === 2) {
            $server->stop();
        }
    }
});

$client = spawn(function () use ($port) {
    $php = getenv('TEST_PHP_EXECUTABLE') ?: PHP_BINARY;
    foreach (['/reset' => 'reset', '/fin' => 'fin'] as $path => $how) {
        delay(50);
        $cmd = sprintf('%s %s %d %s %s 2>&1',
            escapeshellarg($php), escapeshellarg(__DIR__ . '/_tls_peer_gone_client.inc'), $port, $path, $how);
        $proc = proc_open($cmd, [1 => ['pipe', 'w']], $pipes);
        while (proc_get_status($proc)['running']) {
            delay(20);
        }
        $out = stream_get_contents($pipes[1]);
        if ($out !== '') {
            echo "client: $out";
        }
        proc_close($proc);
        delay(200);
    }
});

spawn(function () use ($server) {
    delay(8000);
    if ($server->isRunning()) {
        echo "safety stop\n";
        $server->stop();
    }
});

$server->start();
await($client);
@unlink($cert); @unlink($key); @rmdir($tmp_dir);
echo "server survived\n";
?>
--EXPECT--
/reset: caught HttpException 499
/reset: finally in time
/fin: caught HttpException 499
/fin: finally in time
server survived
