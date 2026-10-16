--TEST--
Rate limit (#414): HTTP/3 answers 429 with Retry-After, and a refused gRPC call over HTTP/3 ends with grpc-status 8
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h3/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* The server runs HTTP/3 on its own thread. One request a minute under /h3
 * and one under /svc.Greeter: the second GET is refused with 429 and
 * Retry-After, the second gRPC call keeps its 200 and carries grpc-status 8,
 * and neither refusal reaches a handler. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/../h3/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$tmp = __DIR__ . '/tmp-099';
@mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
$frame = "$tmp/frame.bin";
file_put_contents($frame, "\x00" . pack('N', 4) . 'ping');
register_shutdown_function(function () use ($tmp, $cert, $key, $frame) {
    @unlink($cert); @unlink($key); @unlink($frame); @rmdir($tmp);
});

$port = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key));

$server->addRateLimit((new RateLimit(1, 60))->paths('/h3')->name('h3'));
$server->addRateLimit((new RateLimit(1, 60))->paths('/svc.Greeter')->name('grpc'));

$runs = 0;
$server->addHttpHandler(function ($req, $res) use (&$runs) { $runs++; $res->setBody('ok'); });
$server->addGrpcHandler(function ($req, $res) use (&$runs) {
    $runs++;
    $req->awaitBody();
    $res->writeMessage('pong');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

function h3(string $bin, int $port, string $path, string $method = 'GET', ?string $body = null, ?string $header = null): string
{
    $env = 'H3CLIENT_DEADLINE_MS=4000 H3CLIENT_PRINT_HEADERS=1 '
         . ($header !== null ? 'H3CLIENT_HEADER=' . escapeshellarg($header) . ' ' : '');
    $out = shell_exec(sprintf('%s%s 127.0.0.1 %d %s %s %s 2>&1 >/dev/null', $env,
        escapeshellarg($bin), $port, escapeshellarg($path), $method,
        $body !== null ? escapeshellarg($body) : '')) ?? '';
    $found = [];

    foreach (explode("\n", $out) as $line) {
        if (preg_match('/^(STATUS=\d+|HDR (retry-after|grpc-status|grpc-message|grpc-retry-pushback-ms): .*)$/', $line)) {
            $found[] = $line;
        }
    }

    return implode(' | ', $found);
}

spawn(function () use ($server, $port, $client_bin, $frame, &$runs) {
    delay(300);
    echo "GET #1: ", h3($client_bin, $port, '/h3'), "\n";
    echo "GET #2: ", h3($client_bin, $port, '/h3'), "\n";
    echo "gRPC #1: ", h3($client_bin, $port, '/svc.Greeter/Ping', 'POST', $frame, 'content-type: application/grpc'), "\n";
    echo "gRPC #2: ", h3($client_bin, $port, '/svc.Greeter/Ping', 'POST', $frame, 'content-type: application/grpc'), "\n";
    echo "handlers run=$runs\n";
    echo "rate_limited_total=", $server->getTelemetry()['requests_rate_limited_total'], "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECT--
GET #1: STATUS=200
GET #2: HDR retry-after: 60 | STATUS=429
gRPC #1: HDR grpc-status: 0 | STATUS=200
gRPC #2: HDR grpc-status: 8 | HDR grpc-message: rate limit exceeded | HDR grpc-retry-pushback-ms: 60000 | STATUS=200
handlers run=2
rate_limited_total=2
done
