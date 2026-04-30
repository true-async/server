--TEST--
HttpServer: HTTP/3 graceful CONNECTION_CLOSE on server stop (Step 6a)
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
/* Step 6a — graceful connection lifecycle.
 *
 * h3client issues a clean QUIC close after the GET. ngtcp2 sees the
 * peer CONNECTION_CLOSE and moves into the draining period; the next
 * read_pkt returns and our post-IO reaper unhooks + frees the conn.
 * In draining we MUST NOT emit a close, so close_sent stays 0; reaped
 * advances to 1. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-109';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$port = 20400 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('ok');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin) {
    usleep(80000);

    $cmd = sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1;
    echo "status=$status\n";

    /* Give the reactor a chance to deliver the peer's CONNECTION_CLOSE
     * datagram and run the reap path. */
    \Async\delay(150);

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "in_draining=",  (int)($s['quic_conn_in_draining']     ?? -1), "\n";
    echo "close_sent=",   (int)($s['quic_connection_close_sent'] ?? -1), "\n";
    echo "reaped=",       (int)($s['quic_conn_reaped']          ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
in_draining=1
close_sent=0
reaped=1
done
