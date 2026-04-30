--TEST--
HttpServer: repeated INITIAL with same DCID routes to the same connection
--EXTENSIONS--
true_async_server
true_async
sockets
--ENV--
PHP_HTTP3_DISABLE_RETRY=1
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['sockets' => true, 'openssl_cli' => true]);
?>
--FILE--
<?php
/* Retransmitted INITIAL packets from a real client carry the same DCID
 * as the first one until the server responds with a fresh SCID.
 * Without the DCID → connection map, each retransmit would spawn a new
 * ngtcp2_conn (quic_conn_accepted++); the map must short-circuit the
 * second Initial to the already-accepted conn. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-102';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';
if (!h3_gen_cert($key_path, $cert_path)) { echo "cert gen failed\n"; exit(1); }

$port = 20150 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert_path)->setPrivateKey($key_path);
$server = new HttpServer($config);
$server->addHttpHandler(fn($r, $s) => $s->setStatusCode(200)->setBody('x'));

$client = spawn(function () use ($server, $port) {
    usleep(50000);

    /* Pin the DCID so all three Initials address the same conn. */
    $dcid = "\xDE\xAD\xBE\xEF\x00\x01\x02\x03";
    $scid = "\xAA\xBB\xCC\xDD";
    $mk = fn($ver) =>
        chr(0xC0) . pack('N', $ver)
        . chr(strlen($dcid)) . $dcid
        . chr(strlen($scid)) . $scid
        . chr(0) . chr(0x40) . chr(0x10)
        . chr(0) . str_repeat("\x00", 15);

    $initial = $mk(1);
    $initial .= str_repeat("\x00", 1250 - strlen($initial));

    $sock = socket_create(AF_INET, SOCK_DGRAM, SOL_UDP);
    for ($i = 0; $i < 3; $i++) {
        socket_sendto($sock, $initial, strlen($initial), 0, '127.0.0.1', $port);
        usleep(10000);
    }
    socket_close($sock);
    usleep(50000);

    $s = $server->getHttp3Stats()[0] ?? [];
    /* We expect exactly ONE conn even though three Initials arrived. */
    echo "conn_accepted=", $s['quic_conn_accepted']   ?? -1, "\n";
    echo "initial_total=", $s['quic_initial']         ?? -1, "\n";
    echo "read_touched_1plus=",
         (int)(($s['quic_read_ok']    ?? 0)
             + ($s['quic_read_error'] ?? 0)
             + ($s['quic_read_fatal'] ?? 0) >= 1), "\n";

    $server->stop();
});

$server->start();
await($client);
@unlink($cert_path); @unlink($key_path); @rmdir($tmp_dir);
echo "done\n";
?>
--EXPECT--
conn_accepted=1
initial_total=3
read_touched_1plus=1
done
