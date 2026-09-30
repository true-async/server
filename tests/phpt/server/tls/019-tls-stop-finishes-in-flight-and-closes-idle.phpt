--TEST--
HttpServer TLS: stop() lets a large response and a sendFile() in flight finish, and closes an idle connection (#345)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['openssl_cli' => true, 'curl' => true, 'proc_open' => true, 'php_ssl' => true]);
?>
--FILE--
<?php
/* stop() takes every connection out of service. Over TLS a connection running
 * a handler closes the way a non-keep-alive response does, with close_notify
 * behind the data already queued, so a body larger than the TLS ring still
 * arrives whole; a connection with nothing in flight closes at once. Three
 * cases: a 1 MiB setBody() and a 1 MiB sendFile() from handlers that call
 * stop() first, and a keep-alive connection that sends its second request
 * after start() has returned. */

require_once __DIR__ . '/_tls_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-019';
if (!is_dir($tmp)) { mkdir($tmp, 0700, true); }
$cert = "$tmp/cert.pem";
$key  = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) { echo "cert generation failed\n"; exit(1); }

$size = 1024 * 1024;
$body = '';
for ($i = 0; $i < $size / 1024; $i++) {
    $body .= str_repeat(chr(65 + $i % 26), 1024);
}

$file = "$tmp/body.bin";
file_put_contents($file, $body);

function serve(int $port, string $cert, string $key, callable $handler): HttpServer
{
    $server = new HttpServer((new HttpServerConfig())
        ->addListener('127.0.0.1', $port, true)
        ->enableTls(true)
        ->setCertificate($cert)
        ->setPrivateKey($key)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
        ->setShutdownTimeout(2));
    $server->addHttpHandler($handler);

    return $server;
}

function curl_get(int $port, string $tmp): array
{
    $out = "$tmp/out.bin";
    $hdr = "$tmp/headers.txt";
    @unlink($out);
    @unlink($hdr);
    shell_exec(sprintf('curl -sk --http1.1 -m 8 -D %s -o %s https://127.0.0.1:%d/ 2>&1',
        escapeshellarg($hdr), escapeshellarg($out), $port));

    return [(string) @file_get_contents($hdr), (string) @file_get_contents($out)];
}

foreach (['setBody', 'sendFile'] as $mode) {
    $port = tas_free_port();
    $server = null;
    $server = serve($port, $cert, $key, function ($req, $res) use (&$server, $mode, $body, $file) {
        $server->stop();
        $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
        $mode === 'setBody' ? $res->setBody($body) : $res->sendFile($file);
    });
    $client = spawn(fn() => curl_get($port, $tmp));
    $server->start();
    [$headers, $got] = await($client);
    echo "$mode: ", strlen($got), " of $size bytes, ", $got === $body ? 'intact' : 'CORRUPT', ", ",
        stripos($headers, "connection: close") !== false ? 'Connection: close' : 'no close', "\n";
}

/* Idle: the child answers its first request, waits for a line on stdin, and
 * sends the second request on the same connection. */
$child = "$tmp/child.php";
file_put_contents($child, <<<'PHP'
<?php
$ctx = stream_context_create(['ssl' => ['verify_peer' => false, 'verify_peer_name' => false]]);
$c = stream_socket_client("ssl://127.0.0.1:{$argv[1]}", $errno, $errstr, 4, STREAM_CLIENT_CONNECT, $ctx);
stream_set_timeout($c, 4);
fwrite($c, "GET /a HTTP/1.1\r\nHost: t\r\n\r\n");
$head = '';
while (!str_contains($head, "\r\n\r\nok")) {
    $chunk = fread($c, 4096);
    if ($chunk === false || $chunk === '') { break; }
    $head .= $chunk;
}
echo "before stop ", str_starts_with($head, "HTTP/1.1 200") ? 'answered' : 'missing', "\n";
fgets(STDIN);
@fwrite($c, "GET /b HTTP/1.1\r\nHost: t\r\n\r\n");
stream_set_timeout($c, 2);
$in = (string) @stream_get_contents($c);
echo "after stop ", substr_count($in, "HTTP/1.1 200"), " answered, ", feof($c) ? 'closed' : 'open', "\n";
PHP);

$port = tas_free_port();
$server = serve($port, $cert, $key, function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});
$proc = null;
$pipes = [];
$client = spawn(function () use ($port, $server, $child, &$proc, &$pipes) {
    $proc = proc_open([PHP_BINARY, '-n', $child, (string) $port],
        [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
    echo "idle: ", fgets($pipes[1]);
    $server->stop();
});
$server->start();
await($client);
fwrite($pipes[0], "go\n");
fclose($pipes[0]);
echo "idle: ", stream_get_contents($pipes[1]);
echo stream_get_contents($pipes[2]);
fclose($pipes[1]);
fclose($pipes[2]);
proc_close($proc);

foreach (['cert.pem', 'key.pem', 'body.bin', 'out.bin', 'headers.txt', 'child.php'] as $f) {
    @unlink("$tmp/$f");
}
@rmdir($tmp);
?>
--EXPECT--
setBody: 1048576 of 1048576 bytes, intact, Connection: close
sendFile: 1048576 of 1048576 bytes, intact, Connection: close
idle: before stop answered
idle: after stop 0 answered, closed
