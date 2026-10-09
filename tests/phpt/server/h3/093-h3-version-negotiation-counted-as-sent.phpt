--TEST--
HttpServer: HTTP/3 counts a Version Negotiation reply in quic_bytes_sent and quic_packets_sent
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
/* A Version Negotiation reply has no connection behind it; it leaves through
 * the listener's send call like every other datagram. The client is the
 * oracle: the server sent this one datagram and nothing else, so the send
 * counters must read its length and one packet, as h3/018 forges it. */

use TrueAsync\HttpServer;
require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-093';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1, true)
    ->addHttp3Listener('127.0.0.1', $port)
    ->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('x'); });

$client = spawn(function () use ($server, $port) {
    \Async\delay(80);

    /* Build minimum viable long-header Initial:
     *   - byte 0 = 0xc0 (form=1, fixed=1, type=Initial, packet-number-len=1)
     *   - bytes 1..4 = version 0xdeadbeef
     *   - byte 5    = DCID len = 8
     *   - bytes 6..13 = DCID
     *   - byte 14   = SCID len = 8
     *   - bytes 15..22 = SCID
     * Length / token / payload aren't parsed before version, so we can
     * stop after the SCID and the server's pkt_decode_version_cid will
     * still recognise it as long-form unknown-version. */
    $pkt  = chr(0xc0);
    $pkt .= "\xde\xad\xbe\xef";
    $pkt .= chr(8) . random_bytes(8);
    $pkt .= chr(8) . random_bytes(8);
    /* Pad to 1200 bytes — RFC 9000 §14.1 requires Initial-bearing
     * datagrams be >= 1200 anti-amplification bytes; our dispatch
     * doesn't enforce this for VN trigger but a real client would. */
    $pkt = str_pad($pkt, 1200, "\x00");

    $sock = stream_socket_client(
        "udp://127.0.0.1:$port", $errno, $errstr, 1, STREAM_CLIENT_CONNECT);
    if (!$sock) { echo "udp connect failed: $errstr\n"; return; }
    stream_set_blocking($sock, false);

    fwrite($sock, $pkt);
    \Async\delay(120);
    $reply = stream_get_contents($sock);
    fclose($sock);

    /* VN reply: byte 0 has form=1, version=0 (bytes 1..4 == 0). */
    $is_vn = false;
    if (is_string($reply) && strlen($reply) >= 7) {
        $form_bit = (ord($reply[0]) & 0x80) !== 0;
        $ver = substr($reply, 1, 4);
        $is_vn = $form_bit && ($ver === "\x00\x00\x00\x00");
    }
    echo "got_vn=", $is_vn ? 1 : 0, "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    $len = is_string($reply) ? strlen($reply) : 0;
    echo "bytes_sent equals reply: ", (int)($s['quic_bytes_sent'] ?? -1) === $len ? 'yes'
        : "no ({$s['quic_bytes_sent']} counted, $len received)", "\n";
    echo "packets_sent: ", (int)($s['quic_packets_sent'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
got_vn=1
bytes_sent equals reply: yes
packets_sent: 1
done
