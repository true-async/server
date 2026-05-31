--TEST--
HttpServer: HTTP/3 large buffered response body (2 MiB) — byte integrity, no flow-control stall
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
/* A buffered (setBody) response far larger than one stream flow-control
 * window must stream out across multiple MAX_STREAM_DATA / ACK-driven
 * resume cycles and arrive byte-exact. Earlier the h3client harness
 * advertised only a 1 MiB stream window and never grew it, so anything
 * above ~1 MiB stalled (200 + 0 body) — a *client* limitation, not the
 * server: the same server serves 4–8 MiB fine to a larger-window client.
 * This pins it at 2 MiB so a regression in either the server's
 * window-resume path or the harness window shows up as a stall/mismatch. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-028';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$port = 20440 + getmypid() % 40;

/* Position-varying content so a reorder/truncation shows as a sha1 miss. */
$size = 2 * 1024 * 1024;
$body = str_repeat("\0", $size);
for ($i = 0; $i < $size; $i++) { $body[$i] = chr(($i * 31 + 7) & 0xff); }
$want_sha = sha1($body);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($body) {
    $res->setBody($body);
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $size, $want_sha) {
    usleep(120000);
    $cmd = sprintf('%s 127.0.0.1 %d / GET 2>/dev/null',
        escapeshellarg($client_bin), $port);
    $b = shell_exec($cmd) ?? '';
    printf("len=%d match=%d\n", strlen($b),
        (strlen($b) === $size && sha1($b) === $want_sha) ? 1 : 0);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
len=2097152 match=1
done
