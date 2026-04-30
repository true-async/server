--TEST--
HttpServer: getHttp3Stats() exposes Step-4 handshake / ALPN / nghttp3 counters
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
/* Step 4.0 added a handshake_completed ngtcp2 callback that performs
 * defense-in-depth ALPN verification. It bumps two new stat counters:
 *   - quic_handshake_completed (success path)
 *   - quic_alpn_mismatch        (peer negotiated non-h3)
 *
 * Without a real h3 client we cannot drive a successful handshake here,
 * but we CAN assert the counters appear in the stats schema with value 0.
 * That alone catches the regression of "someone removed the counter from
 * the binding" or "renamed the key". */

require __DIR__ . '/_h3_skipif.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp_dir = __DIR__ . '/tmp-105';
if (!is_dir($tmp_dir)) { mkdir($tmp_dir, 0700, true); }
$cert_path = $tmp_dir . '/cert.pem';
$key_path  = $tmp_dir . '/key.pem';
if (!h3_gen_cert($key_path, $cert_path)) { echo "cert gen failed\n"; exit(1); }

$port = 20250 + getmypid() % 40;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert_path)->setPrivateKey($key_path);
$server = new HttpServer($config);
$server->addHttpHandler(fn($r, $s) => $s->setStatusCode(200)->setBody('x'));

$probe = spawn(function () use ($server) {
    usleep(50000);
    $stats = $server->getHttp3Stats()[0] ?? [];
    echo "has_handshake_completed=", array_key_exists('quic_handshake_completed', $stats) ? 'yes' : 'no', "\n";
    echo "has_alpn_mismatch=",       array_key_exists('quic_alpn_mismatch', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_init_ok=",          array_key_exists('h3_init_ok', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_init_failed=",      array_key_exists('h3_init_failed', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_stream_close=",     array_key_exists('h3_stream_close', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_stream_read_error=",array_key_exists('h3_stream_read_error', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_request_received=", array_key_exists('h3_request_received', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_request_oversized=",array_key_exists('h3_request_oversized', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_streams_opened=",   array_key_exists('h3_streams_opened', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_response_submitted=",  array_key_exists('h3_response_submitted', $stats) ? 'yes' : 'no', "\n";
    echo "has_h3_response_submit_error=", array_key_exists('h3_response_submit_error', $stats) ? 'yes' : 'no', "\n";
    echo "handshake_completed=", (int)($stats['quic_handshake_completed'] ?? -1), "\n";
    echo "alpn_mismatch=",       (int)($stats['quic_alpn_mismatch'] ?? -1), "\n";
    echo "h3_init_ok=",          (int)($stats['h3_init_ok'] ?? -1), "\n";
    $server->stop();
});

$server->start();
await($probe);

@unlink($cert_path); @unlink($key_path); @rmdir($tmp_dir);
echo "done\n";
?>
--EXPECT--
has_handshake_completed=yes
has_alpn_mismatch=yes
has_h3_init_ok=yes
has_h3_init_failed=yes
has_h3_stream_close=yes
has_h3_stream_read_error=yes
has_h3_request_received=yes
has_h3_request_oversized=yes
has_h3_streams_opened=yes
has_h3_response_submitted=yes
has_h3_response_submit_error=yes
handshake_completed=0
alpn_mismatch=0
h3_init_ok=0
done
