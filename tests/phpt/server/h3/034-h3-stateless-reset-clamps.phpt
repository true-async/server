--TEST--
HttpServer: HTTP/3 stateless-reset size boundaries — reject <41, reply clamped to [22,1200] (#59)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true]);
?>
--FILE--
<?php
/* Test 013 covers one happy-path stateless reset. RFC 9000 §10.3 also
 * pins the SIZE envelope, which is security-load-bearing: a reply to a
 * too-small probe is an amplification vector, and the wrong reply size
 * lets a peer fingerprint a reset. This drives the boundaries of
 * http3_packet_send_stateless_reset():
 *   - inbound < 41  -> NO reply (reject), reset counter unchanged;
 *   - reply length  = clamp(inbound - 1, 22, 1200).
 * Cases: 40 (reject), 41 (->40), 60 (->59), 1300 (->1200 ceiling). */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-034';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }
register_shutdown_function(function () use ($tmp, $cert, $key) {
    @unlink($cert); @unlink($key); @rmdir($tmp);
});

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('x'); });

/* Build a fake 1-RTT (short-header) packet of total size $n to an
 * unknown DCID: byte0 with fixed bit set (0x40), 8-byte DCID, padding. */
function h3_short_pkt(int $n): string {
    return chr(0x40) . random_bytes(8) . random_bytes($n - 1 - 8);
}

$client = spawn(function () use ($server, $port) {
    \Async\delay(80);
    $sock = stream_socket_client("udp://127.0.0.1:$port", $e, $s, 1, STREAM_CLIENT_CONNECT);
    if (!$sock) { echo "udp connect failed\n"; return; }
    stream_set_blocking($sock, false);

    $lens = [];
    foreach ([40, 41, 60, 1300] as $size) {
        @stream_get_contents($sock);            /* drain any stragglers */
        fwrite($sock, h3_short_pkt($size));
        \Async\delay(100);
        $reply = (string) @stream_get_contents($sock);
        $lens[$size] = strlen($reply);
    }

    echo "reply_40=",   $lens[40],   "\n";   /* reject -> 0 */
    echo "reply_41=",   $lens[41],   "\n";   /* 41-1 = 40 */
    echo "reply_60=",   $lens[60],   "\n";   /* 60-1 = 59 */
    echo "reply_1300=", $lens[1300], "\n";   /* clamp -> 1200 */

    $st = $server->getHttp3Stats()[0] ?? [];
    echo "reset_sent=",   (int)($st['quic_stateless_reset_sent'] ?? -1), "\n"; /* 3 (not the 40) */
    echo "short_header=", (int)($st['quic_short_header'] ?? -1), "\n";         /* 4 */

    fclose($sock);
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
reply_40=0
reply_41=40
reply_60=59
reply_1300=1200
reset_sent=3
short_header=4
done
