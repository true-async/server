--TEST--
HttpServer: 3 GETs on one TLS keep-alive connection
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_tls_skipif.inc';
tls_skipif(['proc_open' => true, 'openssl_cli' => true, 'curl' => true]);
?>
--FILE--
<?php
require_once __DIR__ . '/_tls_skipif.inc';
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-064';
if (!is_dir($tmp)) mkdir($tmp, 0700, true);
$cert = "$tmp/cert.pem"; $key = "$tmp/key.pem";
if (!tls_gen_cert($key, $cert)) {
    echo "cert generation failed
";
    exit(1);
}

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port, true)
    ->enableTls(true)
    ->setCertificate($cert)
    ->setPrivateKey($key)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setKeepAliveTimeout(30);

$server = new HttpServer($config);
$request_count = 0;
/* Peer port per request. Three requests over one kept-alive connection share a
 * port; three fresh connections get three. This is the server's own view of
 * reuse — which is what the test is about, and what curl's verbose log only
 * reports at second hand, in wording that changes between curl releases. */
$peer_ports = [];
$server->addHttpHandler(function ($req, $res) use (&$request_count, &$peer_ports, $server) {
    $request_count++;
    $peer_ports[] = $req->getRemotePort();
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setHeader('X-Seq', (string)$request_count)
        ->setBody("r$request_count:" . $req->getUri())
        ->end();
    if ($request_count === 3) {
        $server->stop();
    }
});

$client = spawn(function () use ($port) {
    usleep(50000);
    /* curl --next fires 3 requests in sequence on the SAME connection.
     * Without --next, curl still keeps the conn alive by default,
     * but --next makes the reuse explicit and comprehensible. */
    $cmd = sprintf(
        'curl -kv --http1.1 -m 5 '
        . 'https://127.0.0.1:%d/first '
        . '--next -kv https://127.0.0.1:%d/second '
        . '--next -kv https://127.0.0.1:%d/third 2>&1',
        $port, $port, $port
    );
    return shell_exec($cmd);
});

spawn(function () use ($server) {
    usleep(3000000);
    if ($server->isRunning()) $server->stop();
});

$server->start();
$out = await($client);

echo "count: $request_count\n";
echo "connections: ", count(array_unique($peer_ports)), "\n";
echo "bodies: "
     . (strpos($out, 'r1:/first')  !== false ? '1' : '_')
     . (strpos($out, 'r2:/second') !== false ? '2' : '_')
     . (strpos($out, 'r3:/third')  !== false ? '3' : '_') . "\n";

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "Done\n";
--EXPECT--
count: 3
connections: 1
bodies: 123
Done
