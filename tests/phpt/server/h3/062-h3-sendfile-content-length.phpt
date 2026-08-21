--TEST--
HttpResponse::sendFile() over HTTP/3 states the size of the file it sends
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'aioquic' => true]);
?>
--ENV--
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* The static pump reads the file straight into the chunk queue, so the
 * response object never holds the bytes and its buffer measures nothing. The
 * count comes from the stat the engine already did, and it is what tells the
 * client how large the download is before it finishes.
 *
 * The buffered route is the control: it holds its body, so its count is
 * measured from the buffer, and both arrive in the same field section. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-062';
@mkdir($dir, 0700, true);
$cert = "$dir/cert.pem";
$key  = "$dir/key.pem";
$file = "$dir/asset.bin";
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$payload = str_repeat('x', 4096);
file_put_contents($file, $payload);

register_shutdown_function(function () use ($dir, $cert, $key, $file) {
    @unlink($cert); @unlink($key); @unlink($file); @rmdir($dir);
});

$probe = __DIR__ . '/../../../h3client/h3probe.py';
$port  = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setReadTimeout(10)->setWriteTimeout(10);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $res) use ($file) {
    if ($req->getUri() === '/asset') {
        $res->sendFile($file);
        return;
    }

    $res->setHeader('Content-Type', 'text/plain')->setBody('payload')->end();
});

spawn(function () use ($server, $port, $probe) {
    usleep(300000);

    $run = static function (string $path) use ($probe, $port) {
        $cmd = sprintf('python3 %s 127.0.0.1 %d %s 2>/dev/null',
            escapeshellarg($probe), $port, escapeshellarg($path));
        $out = (string) shell_exec($cmd);

        return [
            preg_match('/status=(\S+)/', $out, $m) ? $m[1] : '?',
            preg_match('/bytes=(\d+)/', $out, $m) ? (int) $m[1] : -1,
            preg_match('/cl=(\S+)/', $out, $m) ? $m[1] : '?',
            preg_match('/outcome=(.+)$/m', $out, $m) ? trim($m[1]) : '?',
        ];
    };

    [$status, $bytes, $cl, $outcome] = $run('/asset');
    echo "sendfile: status=$status cl=$cl bytes=$bytes outcome=$outcome\n";

    [$status, $bytes, $cl, $outcome] = $run('/buffered');
    echo "buffered: status=$status cl=$cl bytes=$bytes outcome=$outcome\n";

    $server->stop();
});

$server->start();
echo "Done\n";
?>
--EXPECT--
sendfile: status=200 cl=4096 bytes=4096 outcome=CLEAN_END
buffered: status=200 cl=7 bytes=7 outcome=CLEAN_END
Done
