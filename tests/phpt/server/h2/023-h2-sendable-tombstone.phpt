--TEST--
HttpResponse::sendable() — the tombstone throws on a live stream
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* sendable() answered liveness and queue depth with one bool, and a loop
 * reading it as liveness truncated a stream that was merely slow
 * (YanGusik/laravel-spawn#60). The declaration outlives the method for one
 * minor release so shipped adapter code is told what to call instead.
 *
 * Asserted here rather than on a fresh response because HTTP/2 with a filling
 * ring is where sendable() used to return a meaningful answer: the throw is
 * unconditional, not a side effect of a detached or closed response. write()
 * still delivers every byte, so the byte-exact hash holds alongside it.
 *
 * Pure-PHP H2 client (_h2_client.inc) — same reason as 015/022. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$CHUNK_SZ = 8192;
$N_CHUNKS = 48;

$expected = '';
for ($i = 0; $i < $N_CHUNKS; $i++) {
    $expected .= str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ);
}

/* Shared with the handler — what the tombstone raised. */
$obs = ['class' => '', 'message' => ''];

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(15)
    ->setWriteTimeout(15);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($CHUNK_SZ, $N_CHUNKS, &$obs) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
    for ($i = 0; $i < $N_CHUNKS; $i++) {
        try {
            $res->sendable();
            $obs['class'] = 'NO-THROW';
        } catch (\Throwable $e) {
            $obs['class']   = get_class($e);
            $obs['message'] = $e->getMessage();
        }
        $res->write(str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ));
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

echo "class=", $obs['class'], "\n";
echo "message=", $obs['message'], "\n";
echo "done\n";
?>
--EXPECT--
status=200
len=393216
ended=1
hash_match=1
class=TrueAsync\HttpServerRuntimeException
message=sendable() is gone: it answered liveness and queue depth with one bool. Use isWritable() for liveness, tryWrite()/awaitWritable() for room
done
