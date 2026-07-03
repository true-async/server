--TEST--
HttpResponse SSE API — sseStart() with no event commits an empty 200 (HTTP/3)
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
/* sseStart() then close with no event: the first append_chunk (which
 * lazily submits the HEADERS frame) never runs, so without the fix the H3
 * stream carried no response at all and the client saw a header-less
 * stream. h3_stream_mark_ended now submits an empty streaming response
 * (HEADERS + immediate EOF, no chunk_queue / no body) so the peer gets a
 * valid 200 text/event-stream — mirroring H1/H2. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-042';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port_span(2);
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key);
$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->sseStart();   // no event, no comment
    $res->end();
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $tmp) {
    usleep(80000);
    $errf = $tmp . '/err.txt';
    $cmd = sprintf('H3CLIENT_VERBOSE_HEADERS=1 %s 127.0.0.1 %d /events GET 2>%s',
        escapeshellarg($client_bin), $port, escapeshellarg($errf));
    $body = shell_exec($cmd) ?? '';
    $err  = @file_get_contents($errf) ?: '';

    $status  = preg_match('/STATUS=(\d+)/',  $err, $m) ? (int)$m[1] : -1;
    $headers = preg_match('/HEADERS=(\d+)/', $err, $m) ? (int)$m[1] : -1;

    echo "status=", $status, "\n";
    echo "body_empty=", $body === '' ? 'yes' : 'no', "\n";
    echo "headers_ge_3=", $headers >= 3 ? "ok" : "bad ($headers)", "\n";

    $s = $server->getHttp3Stats()[0] ?? [];
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
body_empty=yes
headers_ge_3=ok
response_submitted=1
done
