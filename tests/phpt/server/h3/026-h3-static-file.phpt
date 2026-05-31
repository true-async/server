--TEST--
HttpServer: HTTP/3 static file delivery via sendFile() — byte integrity across the 16 KiB chunk boundary
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
/* Exercises the H3 static pump (http3_static_response.c) via
 * HttpResponse::sendFile(): the pump reads the file in 16 KiB chunks
 * straight into per-chunk zend_strings and pushes them through the
 * streaming path. This pins byte-integrity at the sizes that stress the
 * chunk loop — exactly on the boundary, one over (a 1-byte trailing
 * chunk), and several full chunks plus a remainder — so a regression in
 * the read-into-ZSTR_VAL / short-read trim shows up.
 *
 * sendFile() (not addStaticHandler) is the path wired to the H3 pump in
 * h3_handler_coroutine_dispose. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-026';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$root = $tmp . '/root';
@mkdir($root, 0700, true);
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

/* Deterministic, position-varying content — catches offset/truncation
 * bugs that a uniform fill would hide. */
$sizes = [1, 16383, 16384, 16385, 49152, 50000];
$sha = [];
foreach ($sizes as $n) {
    $buf = '';
    for ($i = 0; $i < $n; $i++) $buf .= chr(($i * 31 + 7) & 0xff);
    file_put_contents("$root/f$n.bin", $buf);
    $sha[$n] = sha1($buf);
}

register_shutdown_function(function () use ($tmp, $root, $sizes) {
    foreach ($sizes as $n) @unlink("$root/f$n.bin");
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem");
    @rmdir($root); @rmdir($tmp);
});

$port = 20360 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($root) {
    /* Handler is the trust boundary for sendFile(); map the request name
     * onto the file we wrote (basename strips any path trickery). */
    $name = basename($req->getUri());
    $res->sendFile($root . '/' . $name);
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $sizes, $sha) {
    usleep(120000);

    foreach ($sizes as $n) {
        /* stdout = exact body bytes; STATUS/COMPLETED go to stderr. */
        $cmd = sprintf('%s 127.0.0.1 %d /f%d.bin GET 2>/dev/null',
            escapeshellarg($client_bin), $port, $n);
        $body = shell_exec($cmd);
        $body = $body ?? '';
        $ok = (strlen($body) === $n) && (sha1($body) === $sha[$n]);
        printf("size=%d len=%d match=%d\n", $n, strlen($body), $ok ? 1 : 0);
    }

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
size=1 len=1 match=1
size=16383 len=16383 match=1
size=16384 len=16384 match=1
size=16385 len=16385 match=1
size=49152 len=49152 match=1
size=50000 len=50000 match=1
done
