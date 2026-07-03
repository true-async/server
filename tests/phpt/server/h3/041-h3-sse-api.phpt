--TEST--
HttpResponse SSE API — HTTP/3 event stream over QUIC DATA frames
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
/* Server-Sent Events over HTTP/3: the same first-class API drives the
 * h3 stream_ops (lazy HEADERS on first append_chunk, one DATA frame per
 * event, mark_ended → EOF). Verifies the WHATWG framing reassembles
 * byte-for-byte over QUIC and the three SSE headers were emitted
 * (h3client reports the non-status header count). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-041';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$expected_body =
    "data: hello\n\n" .
    "data: multi\ndata: line\n\n" .
    "id: 7\nevent: ping\ndata: named\n\n";

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->sseStart();
    $res->sseEvent("hello");
    $res->sseEvent("multi\nline");
    $res->sseEvent("named", event: "ping", id: "7");
    $res->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $tmp, $expected_body) {
    usleep(80000);
    $errf = $tmp . '/err.txt';
    $cmd = sprintf('H3CLIENT_VERBOSE_HEADERS=1 %s 127.0.0.1 %d /events GET 2>%s',
        escapeshellarg($client_bin), $port, escapeshellarg($errf));
    $body = shell_exec($cmd) ?? '';
    $err  = @file_get_contents($errf) ?: '';

    $status  = preg_match('/STATUS=(\d+)/',  $err, $m) ? (int)$m[1] : -1;
    $headers = preg_match('/HEADERS=(\d+)/', $err, $m) ? (int)$m[1] : -1;

    echo "status=", $status, "\n";
    echo "body=", $body === $expected_body ? "ok" : "bad", "\n";
    echo "headers_ge_3=", $headers >= 3 ? "ok" : "bad ($headers)", "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
    echo "streams_opened=",     (int)($s['h3_streams_opened']     ?? -1), "\n";
    echo "response_submitted=", (int)($s['h3_response_submitted'] ?? -1), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @unlink($tmp . '/err.txt'); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
status=200
body=ok
headers_ge_3=ok
streams_opened=1
response_submitted=1
done
