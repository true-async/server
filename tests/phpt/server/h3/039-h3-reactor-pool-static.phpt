--TEST--
HttpServer: HTTP/3 static file served on the transport reactor + passthrough to worker (#80, #60)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY === 'Windows') die('skip libuv on Windows lacks SO_REUSEPORT');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--ENV--
TRUE_ASYNC_SERVER_REACTOR_POOL=1
PHP_HTTP3_DISABLE_RETRY=1
--FILE--
<?php
/* Reactor-pool static serving (#80, issue #60 in the split).
 *
 * With the gated pool, the C transport reactor owns the H3 listener. A
 * request that matches a StaticHandler mount must be served entirely on the
 * reactor (no PHP, no worker round-trip); a request that does not match must
 * fall through to a PHP worker. Two h3client GETs assert both halves:
 *   /static/hello.txt -> file body  (served on the reactor)
 *   /dyn              -> handler body (passthrough to a worker)
 *
 * Before reactor-side static, the static request fell through to the worker's
 * user handler and never read the file. SIGKILL teardown (issue #11). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';

$tmp = __DIR__ . '/tmp-039';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$root = $tmp . '/www';
@mkdir($root, 0700, true);
file_put_contents("$root/hello.txt", "reactor-static-body");
/* Large file forces the hard-zero sendfile path (vs the inline HANDLED
 * small-file path) — both run on the reactor in the split. */
$big = str_repeat("0123456789abcdef", 16384);   /* 256 KiB */
file_put_contents("$root/big.bin", $big);
$big_sha1 = sha1($big);
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

register_shutdown_function(function () use ($tmp, $cert, $key, $root) {
    @unlink($cert); @unlink($key); @unlink("$root/hello.txt"); @unlink("$root/big.bin");
    @rmdir($root); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key)
    ->setWorkers(2);
$server = new HttpServer($config);
/* setOpenFileCache opts the mount into the per-reactor open-file cache
 * (#80) — the repeated fetches below populate then hit it on the reactor. */
$server->addStaticHandler(
    (new StaticHandler('/static/', $root))->disableIndex()->setOpenFileCache(64, 60));
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('dyn:' . $req->getMethod() . ':' . $req->getUri());
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($port, $client_bin, $big_sha1) {
    usleep(600000);

    $run = function (string $path, int $count = 1) use ($client_bin, $port) {
        $cmd = sprintf('H3CLIENT_REQUEST_COUNT=%d H3CLIENT_DEADLINE_MS=4000 %s 127.0.0.1 %d %s GET 2>&1',
            $count, escapeshellarg($client_bin), $port, escapeshellarg($path));
        $out = shell_exec($cmd) ?? '';
        return preg_replace('/^(?:STATUS=\d+|HEADERS=\d+|COMPLETED=\d+)\n?/m', '', $out);
    };

    /* Two requests on one connection → same reactor: first is a cache miss
     * (insert), second a hit. Both must return the file body. */
    $two = $run('/static/hello.txt', 2);
    echo "static_x2=", (substr_count($two, 'reactor-static-body') === 2 ? "yes" : "no"), "\n";

    $big = $run('/static/big.bin');
    echo "big_match=", (strlen($big) === 262144 && sha1($big) === $big_sha1 ? "yes" : "no len=" . strlen($big)), "\n";
    echo "dynamic=", trim($run('/dyn')), "\n";

    /* Issue #11: no clean cross-thread pool shutdown yet. */
    posix_kill(getmypid(), SIGKILL);
});

$server->start();
?>
--EXPECTF--
%Astatic_x2=yes
big_match=yes
dynamic=dyn:GET:/dyn
%A
