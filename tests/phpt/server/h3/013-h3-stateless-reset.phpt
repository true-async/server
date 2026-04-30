--TEST--
HttpServer: HTTP/3 stateless reset on unknown-DCID short header (Step 6c)
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
/* Step 6c — fabricate a 1-RTT packet (short header, random DCID) and
 * UDP-send it to the H3 port. The listener has no live connection
 * matching that DCID, so the dispatch path takes the stateless-reset
 * branch. We verify:
 *   - the counter quic_stateless_reset_sent advances,
 *   - we receive at least one reply datagram (size >= 22, < inbound,
 *     trailing 16 bytes are deterministic via HMAC-SHA256(sr_key,dcid)
 *     — but we can't recompute that without the key, so we just check
 *     the size envelope and count). */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-110';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
if (!h3_gen_cert($key, $cert)) { echo "cert gen failed\n"; exit(1); }

$port = 20500 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setBody('x'); });

$client = spawn(function () use ($server, $port) {
    \Async\delay(80);

    /* Build a 60-byte fake 1-RTT packet:
     *   - byte 0: header form 0, fixed bit 1 → 0x40 (top two bits)
     *   - bytes 1..8: DCID (8 bytes)
     *   - rest: random
     * Pad to 60 bytes so the server's anti-amplification check (>= 41)
     * is satisfied. */
    $hdr  = chr(0x40);
    $dcid = random_bytes(8);
    $rest = random_bytes(60 - 1 - 8);
    $pkt  = $hdr . $dcid . $rest;

    $sock = stream_socket_client(
        "udp://127.0.0.1:$port", $errno, $errstr, 1, STREAM_CLIENT_CONNECT);
    if (!$sock) { echo "udp connect failed: $errstr\n"; return; }
    stream_set_blocking($sock, false);

    fwrite($sock, $pkt);
    \Async\delay(120);

    $reply = stream_get_contents($sock);
    fclose($sock);

    echo "reply_present=", ($reply !== false && $reply !== '' ? 1 : 0), "\n";
    echo "reply_len_ge22=", (strlen((string)$reply) >= 22 ? 1 : 0), "\n";
    echo "reply_len_lt_inbound=",
        (strlen((string)$reply) < 60 ? 1 : 0), "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "stateless_reset_sent=",
        (int)($s['quic_stateless_reset_sent'] ?? -1), "\n";
    echo "short_header=",
        (int)($s['quic_short_header'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
reply_present=1
reply_len_ge22=1
reply_len_lt_inbound=1
stateless_reset_sent=1
short_header=1
done
