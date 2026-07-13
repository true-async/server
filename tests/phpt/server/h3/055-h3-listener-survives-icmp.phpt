--TEST--
HTTP/3 listener survives an ICMP error from a peer that vanished
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
if (PHP_OS_FAMILY !== 'Linux') die('skip needs IP_RECVERR + loopback ICMP');
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* A datagram with an unsupported QUIC version makes the server answer with
 * Version Negotiation. Send one from a socket we close immediately and that
 * answer lands on a dead port: the kernel returns an ICMP port-unreachable,
 * IP_RECVERR queues it on the listener socket, and epoll reports POLLERR —
 * which libuv answers by dropping the fd from the loop for good. Unhandled,
 * one peer that quits early leaves the listener deaf for the whole process. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;

require __DIR__ . '/_h3_skipif.inc';
require_once __DIR__ . '/../_free_port.inc';

$dir  = __DIR__ . '/tmp-055';
$root = $dir . '/root';
@mkdir($root, 0700, true);
$cert = $dir . '/cert.pem';
$key  = $dir . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$body = str_repeat('s', 1024);
file_put_contents("$root/small.txt", $body);

register_shutdown_function(function () use ($dir, $root, $cert, $key) {
    @unlink("$root/small.txt");
    @unlink($cert); @unlink($key); @rmdir($root); @rmdir($dir);
});

$port = tas_free_port_span(2);

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);

$server = new HttpServer($config);
$server->addStaticHandler(new StaticHandler('/static/', $root));

$client_bin = __DIR__ . '/../../../h3client/h3client';

spawn(function () use ($server, $port, $client_bin, $body) {
    usleep(600000);

    /* Long header, version 0x1a2a3a4a, 8-byte CIDs. ngtcp2 only negotiates on
     * a datagram of at least 1200 bytes (anti-amplification), so pad it. */
    $pkt = chr(0xc0) . "\x1a\x2a\x3a\x4a"
         . chr(8) . str_repeat("\x01", 8)
         . chr(8) . str_repeat("\x02", 8)
         . str_repeat("\x00", 1300);

    for ($i = 0; $i < 3; $i++) {
        $s = stream_socket_client("udp://127.0.0.1:$port", $errno, $errstr, 1);
        fwrite($s, $pkt);
        fclose($s);
    }

    $rearms = 0;
    for ($i = 0; $i < 40; $i++) {
        usleep(50000);
        $rearms = 0;
        foreach ($server->getHttp3Stats() as $l) { $rearms += $l['poll_rearms']; }
        if ($rearms > 0) break;
    }
    echo "rearmed: ", ($rearms > 0 ? 'yes' : 'no'), "\n";

    $out = sys_get_temp_dir() . '/h3-055-body-' . getmypid();
    $cmd = sprintf('H3CLIENT_DEADLINE_MS=8000 %s 127.0.0.1 %d /static/small.txt GET 2>&1 >%s',
        escapeshellarg($client_bin), $port, escapeshellarg($out));
    $err = shell_exec($cmd) ?? '';
    $got = (string)@file_get_contents($out);
    @unlink($out);

    $st = preg_match('/STATUS=(\d+)/', $err, $m) ? (int)$m[1] : 0;
    echo "still_serving: status=$st intact=", ($got === $body ? 1 : 0), "\n";
    echo "done\n";

    $server->stop();
});

$server->start();
?>
--EXPECT--
rearmed: yes
still_serving: status=200 intact=1
done
