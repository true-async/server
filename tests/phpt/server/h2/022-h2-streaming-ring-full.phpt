--TEST--
HttpResponse::send() — streaming fills the 16-slot chunk ring, producer suspends on full
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Streaming backpressure — ring-full path. The handler send()s 64
 * chunks of 8 KiB = 512 KiB. The per-stream chunk ring is fixed at 16
 * slots (H2_CHUNK_RING_SLOTS in src/http2/http2_strategy.c); 64 >> 16
 * forces h2_stream_append_chunk's suspend-on-full branch and the
 * ring-compaction memmove repeatedly across fill -> drain -> refill
 * cycles.
 *
 * The single-threaded scheduler makes the ring-fill deterministic: the
 * handler runs its send() loop uninterrupted until the ring is full and
 * it suspends, so the client physically cannot credit the flow-control
 * window before the suspend has happened at least once.
 *
 * Each chunk uses a distinct byte value, so the byte-exact sha1 catches
 * any lost / reordered / duplicated chunk — not just a length mismatch.
 *
 * Pure-PHP H2 client (_h2_client.inc) — same reason as 015: curl
 * under-credits stream windows. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19890 + getmypid() % 100;

$CHUNK_SZ = 8192;
$N_CHUNKS = 64;

$expected = '';
for ($i = 0; $i < $N_CHUNKS; $i++) {
    $expected .= str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ);
}

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(15)
    ->setWriteTimeout(15);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($CHUNK_SZ, $N_CHUNKS) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
    for ($i = 0; $i < $N_CHUNKS; $i++) {
        $res->send(str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ));
    }
    $res->end();
});

$client = spawn(function () use ($port, $server, $expected) {
    usleep(50000);
    try {
        $cli = new H2TestClient('127.0.0.1', $port, 15);
        $sid = $cli->sendRequest('GET', '/stream', "127.0.0.1:$port");
        [$status, $body, $trailers, $ended] = $cli->collectResponse($sid, true);
        $cli->close();

        echo "status=$status\n";
        echo "len=", strlen($body), "\n";
        echo "ended=", (int)$ended, "\n";
        echo "hash_match=", (sha1($body) === sha1($expected) ? 1 : 0), "\n";
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
status=200
len=524288
ended=1
hash_match=1
done
