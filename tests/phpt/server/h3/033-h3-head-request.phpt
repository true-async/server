--TEST--
HttpServer: HTTP/3 HEAD request returns status + headers but no body (RFC 9110 §9.3.2, #59)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* A HEAD response must carry the same status/headers as GET but no body.
 * The H3 handler path used to emit the body unconditionally (unlike H1);
 * http3_stream_submit_response now suppresses it for HEAD. Drive the same
 * handler with GET (body present) and HEAD (body empty) on one server. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-033';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('content-type', 'text/plain')
        ->setBody('hello world');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$run = function (string $method) use ($client_bin, $port): array {
    $out = shell_exec(sprintf('%s 127.0.0.1 %d / %s 2>&1',
        escapeshellarg($client_bin), $port, $method)) ?? '';
    $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out);
    return [$status, trim($body)];
};

$client = spawn(function () use ($server, $run) {
    usleep(120000);
    [$gs, $gb] = $run('GET');
    [$hs, $hb] = $run('HEAD');
    echo "get_status=$gs\n";
    echo "get_body=$gb\n";
    echo "head_status=$hs\n";
    echo "head_body_len=", strlen($hb), "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
get_status=200
get_body=hello world
head_status=200
head_body_len=0
done
