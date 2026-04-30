--TEST--
HttpServer: addHttp3Listener — UDP datagrams reach the listener (PLAN_HTTP3 Step 1)
--EXTENSIONS--
true_async_server
true_async
sockets
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['sockets' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/* H3 listener accepts datagrams and counts them. Step 3b made the
 * listener require TLS (QUIC mandates TLS 1.3) so we also generate a
 * self-signed cert. Garbage payloads get classified as parse_errors at
 * the QUIC decoder — datagrams_received still ticks at the recv-callback
 * layer before classification, which is what this test asserts. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-098';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';
if (!h3_gen_cert($key_path, $cert_path)) { echo "cert gen failed\n"; exit(1); }

$port = 19900 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)  /* dummy TCP so start() has >=1 listener pre-H3 */
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)
    ->setCertificate($cert_path)
    ->setPrivateKey($key_path)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);

/* start() requires a handler for the TCP side; H3 is handler-less at
 * Step 1 because we don't terminate QUIC yet. */
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('tcp-ok');
});

$client = spawn(function () use ($server, $port) {
    /* Give the reactor a tick to enter recvfrom. */
    usleep(50000);

    $sock = socket_create(AF_INET, SOCK_DGRAM, SOL_UDP);
    if ($sock === false) {
        echo "socket_create failed\n";
        $server->stop();
        return;
    }

    $payloads = ["hello-quic\0", str_repeat("A", 200), "bye"];
    foreach ($payloads as $p) {
        $n = socket_sendto($sock, $p, strlen($p), 0, '127.0.0.1', $port);
        if ($n === false) {
            echo "sendto failed\n";
        }
        /* Small gap so each datagram gets its own reactor tick. */
        usleep(5000);
    }
    socket_close($sock);

    /* Wait long enough for multishot recv to drain all three. */
    usleep(50000);

    $stats = $server->getHttp3Stats();
    $total = strlen($payloads[0]) + strlen($payloads[1]) + strlen($payloads[2]);

    echo "listeners=", count($stats), "\n";
    $s = $stats[0] ?? [];
    echo "host_match=", (int)(($s['host'] ?? '') === '127.0.0.1'), "\n";
    echo "port_match=", (int)(($s['port'] ?? 0) === $port), "\n";
    echo "datagrams_received=", $s['datagrams_received'] ?? -1, "\n";
    echo "bytes_ok=", (int)(($s['bytes_received'] ?? 0) >= $total), "\n";
    echo "last_size=", $s['last_datagram_size'] ?? -1, "\n";
    echo "peer_nonempty=", (int)!empty($s['last_peer']), "\n";

    $server->stop();
});

$server->start();
await($client);
@unlink($cert_path); @unlink($key_path); @rmdir($tmp_dir);
echo "done\n";
?>
--EXPECT--
listeners=1
host_match=1
port_match=1
datagrams_received=3
bytes_ok=1
last_size=3
peer_nonempty=1
done
