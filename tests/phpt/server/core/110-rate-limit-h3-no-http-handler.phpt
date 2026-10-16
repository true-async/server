--TEST--
Rate limit (#414): HTTP/3 refuses with 429 a request that no handler, static mount or gRPC handler would take
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
/* A gRPC-only server over local HTTP/3: a plain request has no handler and
 * gets no answer. The rule still counts it, and once the client is over the
 * limit the request is answered 429 instead. The gRPC call first spends the
 * client's only token. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require __DIR__ . '/../h3/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$tmp = __DIR__ . '/tmp-110';
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

$server->addRateLimit(new RateLimit(1, 60));
$server->addGrpcHandler(function ($req, $res) {
    $req->awaitBody();
    $res->writeMessage('pong');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

function h3(string $bin, int $port, string $path, string $method = 'GET', ?string $body = null, ?string $header = null): string
{
    $env = 'H3CLIENT_DEADLINE_MS=' . (getenv('ZEND_DONT_UNLOAD_MODULES') !== false ? 30000 : 4000) . ' H3CLIENT_PRINT_HEADERS=1 '
         . ($header !== null ? 'H3CLIENT_HEADER=' . escapeshellarg($header) . ' ' : '');
    $out = shell_exec(sprintf('%s%s 127.0.0.1 %d %s %s %s 2>&1 >/dev/null', $env,
        escapeshellarg($bin), $port, escapeshellarg($path), $method,
        $body !== null ? escapeshellarg($body) : '')) ?? '';
    $found = [];

    foreach (explode("\n", $out) as $line) {
        if (!preg_match('/^(STATUS=\d+|HDR (retry-after|grpc-status|grpc-message|grpc-retry-pushback-ms): .*)$/', $line)) {
            continue;
        }

        if (preg_match('/^HDR retry-after: (\d+)$/', $line, $m)) {
            $line = 'HDR retry-after: ' . rl_wait((int) $m[1]);
        } elseif (preg_match('/^HDR grpc-retry-pushback-ms: (\d+)$/', $line, $m)) {
            $line = 'HDR grpc-retry-pushback-ms: ' . rl_wait(intdiv((int) $m[1], 1000)) * 1000;
        }

        $found[] = $line;
    }

    return implode(' | ', $found);
}

spawn(function () use ($server, $port, $client_bin, $frame) {
    delay(getenv('ZEND_DONT_UNLOAD_MODULES') !== false ? 3000 : 300);
    echo "gRPC: ", h3($client_bin, $port, '/svc.Greeter/Ping', 'POST', $frame, 'content-type: application/grpc'), "\n";
    echo "plain GET: ", h3($client_bin, $port, '/plain'), "\n";
    echo "rate_limited_total=", $server->getTelemetry()['requests_rate_limited_total'], "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--EXPECT--
gRPC: HDR grpc-status: 0 | STATUS=200
plain GET: HDR retry-after: 60 | STATUS=429
rate_limited_total=1
done
