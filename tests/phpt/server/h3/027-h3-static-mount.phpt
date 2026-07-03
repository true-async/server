--TEST--
HttpServer: HTTP/3 addStaticHandler mount-routing — byte integrity + index + 404 (issue #60)
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
/* Exercises addStaticHandler delivery over HTTP/3 — the mount-routing
 * gate in http3_stream_dispatch that calls http_static_try_serve before
 * the user handler. This is a *static-only* deployment (a mount, no
 * addHttpHandler), so it also covers the no-PHP-handler 404 synthesis.
 *
 * Byte-integrity is pinned at the sizes that stress the static pump's
 * 16 KiB chunk loop (on the boundary, one over, several full chunks plus
 * a remainder) so a regression in the read-into-ZSTR / short-read trim
 * shows up. Index resolution, a missing file, and a traversal escape
 * verify the FSM's synchronous HANDLED branches reach the wire too. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-027';
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
 * bugs a uniform fill would hide. */
$sizes = [1, 16383, 16384, 16385, 50000];
$sha = [];
foreach ($sizes as $n) {
    $buf = '';
    for ($i = 0; $i < $n; $i++) $buf .= chr(($i * 31 + 7) & 0xff);
    file_put_contents("$root/f$n.bin", $buf);
    $sha[$n] = sha1($buf);
}
file_put_contents("$root/index.html", "<h1>mount index</h1>");

register_shutdown_function(function () use ($tmp, $root, $sizes) {
    foreach ($sizes as $n) @unlink("$root/f$n.bin");
    @unlink("$root/index.html");
    @unlink("$tmp/cert.pem"); @unlink("$tmp/key.pem");
    @rmdir($root); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
/* No addHttpHandler — the mount is the only thing serving requests. */
$server->addStaticHandler(
    (new StaticHandler('/assets/', $root))->setIndexFiles('index.html'));

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $sizes, $sha) {
    usleep(120000);

    /* Byte-integrity: stdout = exact body bytes (STATUS goes to stderr). */
    foreach ($sizes as $n) {
        $cmd = sprintf('%s 127.0.0.1 %d /assets/f%d.bin GET 2>/dev/null',
            escapeshellarg($client_bin), $port, $n);
        $body = shell_exec($cmd) ?? '';
        $ok = (strlen($body) === $n) && (sha1($body) === $sha[$n]);
        printf("size=%d len=%d match=%d\n", $n, strlen($body), $ok ? 1 : 0);
    }

    /* Status-bearing checks: combine stderr so STATUS= is visible. */
    $do = function (string $path) use ($client_bin, $port) {
        $cmd = sprintf('%s 127.0.0.1 %d %s GET 2>&1',
            escapeshellarg($client_bin), $port, escapeshellarg($path));
        $out = shell_exec($cmd) ?? '';
        $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
        $body = trim(preg_replace('/^STATUS=\d+\n?/m', '', $out));
        return [$status, $body];
    };

    [$st, $bd] = $do('/assets/');
    echo "index: status=$st body=$bd\n";

    [$st, $bd] = $do('/assets/nope.bin');
    echo "miss: status=$st\n";

    [$st, $bd] = $do('/assets/../../etc/passwd');
    echo "trav: status=$st\n";

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
size=50000 len=50000 match=1
index: status=200 body=<h1>mount index</h1>
miss: status=404
trav: status=404
done
