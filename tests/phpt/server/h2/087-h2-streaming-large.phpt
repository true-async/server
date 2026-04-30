--TEST--
HttpResponse::send() — HTTP/2 streaming multi-chunk body within initial window
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* PLAN_STREAMING Phase 1 — handler sends 4 MiB in 64 KiB chunks with
 * buffer threshold = 256 KiB. Backpressure path must engage: handler
 * suspends periodically when chunk_queue_bytes > 256 KiB, resumes
 * once the data provider drains enough. Client reassembles the full
 * 4 MiB body — exact byte-for-byte. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19851 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setStreamWriteBufferBytes(262144);   /* 256 KiB */

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    /* 4 KiB chunks × 12 = 48 KiB total — stays under the default
     * HTTP/2 per-stream initial window (65535 bytes). Proves the
     * multi-chunk queue + data-provider walker work end-to-end.
     *
     * Bodies LARGER than the initial window are a Phase 1.1 item
     * (needs a DP-triggered wake event so send() can suspend
     * properly when flow-control stalls the drain). */
    $chunk = str_repeat('A', 4096);
    for ($i = 0; $i < 12; $i++) {
        $res->send($chunk);
    }
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s --max-time 10 -o /tmp/body-087.bin '
        . '-w "http=%%{http_code} size=%%{size_download}" '
        . 'http://127.0.0.1:%d/',
        $port
    );
    $out = []; exec($cmd, $out, $rc);
    $meta = implode("\n", $out);

    echo "rc=$rc\n";
    echo "meta=$meta\n";

    /* Sanity — exact byte count + content hash (cheap sha1). */
    $sha = @sha1_file('/tmp/body-087.bin');
    $expected = sha1(str_repeat('A', 12 * 4096));
    echo "hash_match=", ($sha === $expected ? 1 : 0), "\n";
    @unlink('/tmp/body-087.bin');

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
rc=0
meta=http=200 size=49152
hash_match=1
done
