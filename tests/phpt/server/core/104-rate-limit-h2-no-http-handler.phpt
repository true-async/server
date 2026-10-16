--TEST--
Rate limit (#414): HTTP/2 refuses with 429 a request that no handler, static mount or gRPC handler would take
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* A gRPC-only server: a plain HTTP/2 request has no handler and is dropped
 * without an answer. The rule still counts it, and once the client is over
 * the limit the request is answered 429 instead of being dropped. The first
 * gRPC call spends the client's only token. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit(new RateLimit(1, 60));
$server->addGrpcHandler(function ($req, $res) {
    $req->awaitBody();
    $res->writeMessage('pong');
});

$client = spawn(function () use ($port, $server) {
    delay(50);
    $frame = tempnam(sys_get_temp_dir(), 'rl104');
    file_put_contents($frame, "\x00" . pack('N', 4) . 'ping');
    $base = "http://127.0.0.1:$port";

    /* escapeshellarg: cmd.exe takes single quotes literally. */
    $grpc = shell_exec('curl -s -i --max-time 3 --http2-prior-knowledge -H '
        . escapeshellarg('content-type: application/grpc') . ' -H ' . escapeshellarg('te: trailers')
        . ' --data-binary ' . escapeshellarg("@$frame") . " $base/svc.Greeter/Ping 2>&1") ?? '';
    @unlink($frame);
    echo "gRPC: ", preg_match('/^HTTP\/2 (\d+)/m', $grpc, $m) ? $m[1] : 'none', "\n";

    $plain = shell_exec("curl -s -i --max-time 3 --http2-prior-knowledge $base/plain 2>&1") ?? '';
    echo "plain GET: ", preg_match('/^HTTP\/2 (\d+)/m', $plain, $m) ? $m[1] : 'none',
         preg_match('/^retry-after: (\d+)/mi', $plain, $r) ? ' retry-after=' . rl_wait((int) $r[1]) : '', "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
gRPC: 200
plain GET: 429 retry-after=60
done
