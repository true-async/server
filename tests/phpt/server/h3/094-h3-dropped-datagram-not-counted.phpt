--TEST--
HttpServer: HTTP/3 does not count a datagram the socket dropped with EAGAIN in quic_bytes_sent
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
if (PHP_OS_FAMILY !== 'Linux') die('skip the fault point sits on the Linux sendmsg path');
if (!function_exists('_http_fault_enable')) die('skip needs --enable-fault-injection');
?>
--FILE--
<?php
/* The handler arms a fault point that answers the next sendmsg with EAGAIN,
 * as a full socket does: the response's first datagram is dropped, and QUIC
 * sends it again. A counting UDP relay between the client and the server is
 * the oracle: every datagram the kernel took reached it, so the server's send
 * counters must equal what it forwarded to the client, and the drop shows in
 * quic_send_eagain alone. */
require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/../_byte_relay.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$tmp = __DIR__ . '/tmp-094';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

$point = 'h3/sendmsg/eagain';
$port = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key));
$server->addHttpHandler(function ($req, $res) use ($point) {
    _http_fault_enable($point);
    $res->setStatusCode(200)->setBody('h3-dropped');
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $point) {
    $relay = new TasUdpRelay($port);
    delay(100);
    $out = shell_exec(sprintf('%s 127.0.0.1 %d / GET 2>&1',
        escapeshellarg($client_bin), $relay->port)) ?? '';
    delay(300);
    $s = $server->getHttp3Stats()[0] ?? [];
    $relay->stop();
    $server->stop();

    echo "status=", preg_match('/^STATUS=(\d+)$/m', $out, $m) ? (int)$m[1] : -1, "\n";
    echo "hits: ", _http_fault_hits($point), "\n";
    echo "eagain: ", (int)($s['quic_send_eagain'] ?? -1), "\n";
    echo "bytes equal relay: ", (int)$s['quic_bytes_sent'] === $relay->down ? 'yes'
        : "no ({$s['quic_bytes_sent']} counted, {$relay->down} relayed)", "\n";
    echo "packets equal relay: ", (int)$s['quic_packets_sent'] === $relay->downDatagrams ? 'yes'
        : "no ({$s['quic_packets_sent']} counted, {$relay->downDatagrams} relayed)", "\n";
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
status=200
hits: 1
eagain: 1
bytes equal relay: yes
packets equal relay: yes
done
