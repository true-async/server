--TEST--
HttpServer: HTTP/3 emits VERSION_NEGOTIATION for unknown QUIC versions (Step 8 conformance smoke)
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
/* Step 8 — in-house RFC 9000 §6 conformance smoke.
 *
 * Forge a long-header QUIC packet whose version field is 0xdeadbeef
 * (not v1, not greased) and assert the server emits a Version
 * Negotiation reply (header-form=1, version=0, supported-versions
 * advertised). Exercises the dispatch path's pkt_decode_version_cid
 * branch independently of any real ngtcp2 client. */

use TrueAsync\HttpServer;
require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-115';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$port = 21000 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1, true)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
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
    echo "vn_count=",
        (int)($s['quic_version_negotiated'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
got_vn=1
vn_count=1
done
