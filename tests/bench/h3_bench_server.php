<?php
/* Long-running H3 server for external load generators (h2load --alpn-list=h3).
 * Unlike h3_payload_bench.php it does NOT drive traffic itself — it boots,
 * prints "READY <port>", and serves a fixed-size body until killed.
 *
 * Env:
 *   BENCH_BODY   response body size in bytes (default 1024)
 *   BENCH_PORT   UDP/TCP port for the H3 listener (default 34433)
 *   BENCH_CERT   cert path  (default tests/bench/tmp-bench/cert.pem, auto-gen)
 *   BENCH_KEY    key path   (default tests/bench/tmp-bench/key.pem)
 *
 * Run:
 *   php -d extension_dir=$(pwd)/modules -d extension=true_async_server \
 *       tests/bench/h3_bench_server.php
 */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;

$BODY = (int)(getenv('BENCH_BODY') ?: 1024);
$PORT = (int)(getenv('BENCH_PORT') ?: 34433);

$tmp = __DIR__ . '/tmp-bench';
@mkdir($tmp, 0700, true);
$cert = getenv('BENCH_CERT') ?: $tmp . '/cert.pem';
$key  = getenv('BENCH_KEY')  ?: $tmp . '/key.pem';
if (!is_file($cert)) {
    exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
        . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
        escapeshellarg($key), escapeshellarg($cert)));
}

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $PORT + 1)
    ->addHttp3Listener('127.0.0.1', $PORT)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);

$body = str_repeat('x', $BODY);
$server->addHttpHandler(function ($req, $res) use ($body) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody($body);
});

fwrite(STDERR, sprintf("READY %d body=%d\n", $PORT, $BODY));
$server->start();
