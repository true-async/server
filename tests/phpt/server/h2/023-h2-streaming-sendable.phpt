--TEST--
HttpResponse::sendable() — advisory backpressure check flips under a full ring
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* sendable() is the non-blocking companion to send(): true when send()
 * would accept a chunk without suspending, false when the per-stream
 * staging buffer is full.
 *
 * The handler send()s 48 chunks of 8 KiB. The single-threaded scheduler
 * runs the handler uninterrupted until it suspends, so the client
 * cannot credit the flow-control window meanwhile — the ring fills and
 * sendable() must be observed flipping from true to false. send()
 * itself still delivers every byte (it just blocks once full), so the
 * byte-exact hash must also hold.
 *
 * Pure-PHP H2 client (_h2_client.inc) — same reason as 015/022. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19920 + getmypid() % 100;

$CHUNK_SZ = 8192;
$N_CHUNKS = 48;

$expected = '';
for ($i = 0; $i < $N_CHUNKS; $i++) {
    $expected .= str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ);
}

/* Shared with the handler — observed sendable() states. */
$obs = ['true' => false, 'false' => false];

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(15)
    ->setWriteTimeout(15);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($CHUNK_SZ, $N_CHUNKS, &$obs) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
    for ($i = 0; $i < $N_CHUNKS; $i++) {
        if ($res->sendable()) {
            $obs['true'] = true;
        } else {
            $obs['false'] = true;
        }
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

echo "saw_sendable_true=",  (int)$obs['true'],  "\n";
echo "saw_sendable_false=", (int)$obs['false'], "\n";
echo "done\n";
?>
--EXPECT--
status=200
len=393216
ended=1
hash_match=1
saw_sendable_true=1
saw_sendable_false=1
done
