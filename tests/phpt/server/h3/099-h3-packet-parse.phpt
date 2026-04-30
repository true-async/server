--TEST--
HttpServer: H3 listener classifies QUIC packets by type (PLAN_HTTP3 Step 2)
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
/* Wire-level classification: the recv pipeline identifies QUIC packets
 * by header form and acts without creating an ngtcp2_conn yet. We hand
 * craft three datagrams:
 *
 *   A) a minimal QUIC long-header INITIAL (version V1, RFC 9000 §17.2.2)
 *   B) a QUIC long-header with an unsupported "greasing" version —
 *      should trigger a Version Negotiation response
 *   C) pure random bytes — classified as parse error
 *
 * Then read stats and assert the three counters advanced accordingly.
 * Step 3 will replace the classify-only path with real ngtcp2_conn +
 * TLS handshake; this test proves the dispatch plumbing stands up. */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-099';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';
if (!h3_gen_cert($key_path, $cert_path)) { echo "cert gen failed\n"; exit(1); }

$port = 19950 + getmypid() % 30;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)
    ->setCertificate($cert_path)
    ->setPrivateKey($key_path);
$server = new HttpServer($config);
$server->addHttpHandler(fn($r, $s) => $s->setStatusCode(200)->setBody('tcp-stub'));

$client = spawn(function () use ($server, $port) {
    usleep(50000);

    /* Helper: QUIC long header with version |ver|, zero-length DCID+SCID,
     * then an INITIAL-type packet with empty token and a dummy payload
     * (we never encrypt — server just classifies the header).
     *
     * Wire layout (RFC 9000 §17.2):
     *   byte 0: header form (1) | fixed bit (1) | long-packet-type (2)
     *           | reserved (2) | packet number length (2) = 0xC0 for INITIAL
     *   bytes 1..4: version (big endian)
     *   byte 5: DCID length (0)
     *   byte 6: SCID length (0)
     *   then varint(token length=0), varint(payload length), PN, payload */
    $mkLongHeader = function (int $version) {
        /* ngtcp2_accept rejects Initials with DCID < 8 bytes and no token
         * (RFC 9000 §7.2: server-chosen DCID must be at least 8 bytes).
         * Use an 8-byte DCID and a 4-byte SCID. Token is empty. */
        $dcid = "\x01\x02\x03\x04\x05\x06\x07\x08";          // 8 bytes
        $scid = "\xAA\xBB\xCC\xDD";                          // 4 bytes
        return
            chr(0xC0)                                        // INITIAL type byte
            . pack('N', $version)                            // version
            . chr(strlen($dcid)) . $dcid                     // DCID len + DCID
            . chr(strlen($scid)) . $scid                     // SCID len + SCID
            . chr(0)                                         // token length varint = 0
            . chr(0x40) . chr(0x10)                          // payload length varint = 16
            . chr(0)                                         // packet number (1 byte)
            . str_repeat("\x00", 15);                        // opaque ciphertext
    };

    $sock = socket_create(AF_INET, SOCK_DGRAM, SOL_UDP);

    /* A) valid-version INITIAL. RFC 9000 §14.1 mandates client Initial
     * packets be at least 1200 bytes; ngtcp2_accept enforces this on the
     * server side and rejects shorter ones. Pad to 1250. */
    $initial = $mkLongHeader(0x00000001);                    // NGTCP2_PROTO_VER_V1
    $initial .= str_repeat("\x00", 1250 - strlen($initial));
    socket_sendto($sock, $initial, strlen($initial), 0, '127.0.0.1', $port);
    usleep(10000);

    /* B) unsupported version → VN response expected. QUIC requires the
     * datagram to be at least 1200 bytes (NGTCP2_MAX_UDP_PAYLOAD_SIZE) for
     * the decoder to classify it as VN-worthy; shorter unknown-version
     * datagrams are dropped as potential amplification vectors (RFC 9000
     * §14.1). Pad to 1250 bytes with zeros. */
    $bad = $mkLongHeader(0xBABABABA);
    $bad .= str_repeat("\x00", 1250 - strlen($bad));
    socket_sendto($sock, $bad, strlen($bad), 0, '127.0.0.1', $port);
    usleep(10000);

    /* C) garbage — 1 byte is never a valid QUIC packet */
    $junk = "G";
    socket_sendto($sock, $junk, 1, 0, '127.0.0.1', $port);
    usleep(10000);

    socket_close($sock);
    usleep(50000);

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "datagrams=", $s['datagrams_received'] ?? -1, "\n";
    echo "quic_initial=",             $s['quic_initial']            ?? -1, "\n";
    echo "quic_version_negotiated=",  $s['quic_version_negotiated'] ?? -1, "\n";
    echo "quic_parse_errors=",        $s['quic_parse_errors']       ?? -1, "\n";
    echo "quic_short_header=",        $s['quic_short_header']       ?? -1, "\n";
    /* Step 3a: handcrafted INITIAL should produce a new ngtcp2_conn. */
    echo "quic_conn_accepted=",       $s['quic_conn_accepted']      ?? -1, "\n";
    echo "quic_conn_rejected=",       $s['quic_conn_rejected']      ?? -1, "\n";
    /* Step 3b: ngtcp2_conn_read_pkt reached. The handcrafted Initial
     * has a zero-filled ciphertext so ngtcp2 bails in AEAD decrypt and
     * returns a fatal error — that is success for the test: our wire
     * path reaches crypto. A real ClientHello from curl would produce
     * quic_read_ok=1 instead (Step 3c-level integration test). */
    echo "read_pkt_reached=",
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
datagrams=3
quic_initial=1
quic_version_negotiated=1
quic_parse_errors=1
quic_short_header=0
quic_conn_accepted=1
quic_conn_rejected=0
read_pkt_reached=1
done
