--TEST--
HttpServer: HTTP/3 streaming response — HttpResponse::send() loop, multi-chunk DATA
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
/* Step 5b regression — handler streams a response via $res->send()
 * loop, exercising:
 *   - h3_stream_ops.append_chunk first-call HEADERS commit + queue alloc
 *   - h3_read_data_cb chunk_queue branch + chunk_read_idx walking
 *   - h3_acked_stream_data_cb release-on-ACK lifecycle (Step 5c UAF fix)
 *   - h3_stream_ops.mark_ended → resume_stream → EOF semantics
 *   - cwnd-stall recovery via ACK-driven write_event wake
 *
 * 32 chunks × 1 KiB = 32 KiB exceeds initial QUIC cwnd (~17 KiB
 * cubic), so the data_reader stalls mid-response and only the
 * acked_stream_data_offset_cb wake path can complete it. Reassembled
 * body is hashed end-to-end so a wrong chunk order, a missed slice,
 * or a UAF-corrupted retransmit surfaces as mismatch. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-108';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$chunks = [];
for ($i = 0; $i < 32; $i++) {
    $chunks[] = str_pad("CHUNK-$i:", 1024, '.', STR_PAD_RIGHT);
}
$expected_body = implode('', $chunks);
$expected_sha1 = sha1($expected_body);
$expected_len  = strlen($expected_body);

$port = 20500 + getmypid() % 50;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($chunks) {
    $res->setStatusCode(200)->setHeader('content-type', 'application/octet-stream');
    foreach ($chunks as $c) { $res->send($c); }
    $res->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin,
                                 $expected_sha1, $expected_len) {
    usleep(80000);
    $cmd = sprintf('%s 127.0.0.1 %d /stream GET 2>&1',
        escapeshellarg($client_bin), $port);
    $out = shell_exec($cmd) ?? '';

    $status = null;
    if (preg_match('/^STATUS=(\d+)$/m', $out, $m)) $status = (int)$m[1];
    $body = preg_replace('/^STATUS=\d+\n?/m', '', $out);
    if (substr($body, -1) === "\n") $body = substr($body, 0, -1);

    echo "status=", $status ?? -1, "\n";
    echo "len=",   strlen($body) === $expected_len  ? "ok" : ("bad ".strlen($body)), "\n";
    echo "sha1=",  sha1($body)   === $expected_sha1 ? "ok" : "bad", "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "streams_opened=",     (int)($s['h3_streams_opened']     ?? -1), "\n";
    echo "response_submitted=", (int)($s['h3_response_submitted'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
len=ok
sha1=ok
streams_opened=1
response_submitted=1
done
