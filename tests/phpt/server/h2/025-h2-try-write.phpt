--TEST--
HttpResponse::tryWrite() — false when the ring is full, and a refusal queues nothing
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* tryWrite() is send() without the wait, and awaitWritable() is how a handler
 * waits for the room it was refused. False means the per-stream ring had
 * no room; it says nothing about the client, whose departure arrives as
 * HttpException 499 instead.
 *
 * The handler offers 8 KiB chunks and, whenever one is refused, hands the SAME
 * chunk to send(), which waits for room. The byte-exact body hash is what
 * proves a refusal queued nothing: had the refused chunk been queued too, it
 * would appear twice. The single-threaded scheduler runs the handler until it
 * suspends, so the client cannot credit the window meanwhile and a refusal is
 * guaranteed. */

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

$refused = 0;
$waited = 0;
$fellBack = 0;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(15)
    ->setWriteTimeout(15);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) use ($CHUNK_SZ, $N_CHUNKS, &$refused, &$waited, &$fellBack) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    for ($i = 0; $i < $N_CHUNKS; $i++) {
        $chunk = str_repeat(chr(33 + ($i % 90)), $CHUNK_SZ);

        if (!$res->tryWrite($chunk)) {
            $refused++;

            /* Wait for room instead of spinning, then offer the same bytes
             * again. If awaitWritable() returned without waiting, the retry
             * below would be refused too and $fellBack would rise — which is
             * what the expected output rules out. */
            if ($res->awaitWritable(5000)) {
                $waited++;
            }

            if (!$res->tryWrite($chunk)) {
                $fellBack++;
                $res->send($chunk);
            }
        }
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
        echo "hash_match=", (sha1($body) === sha1($expected) ? 1 : 0), "\n";
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }
    $server->stop();
});

$server->start();
await($client);

echo "refused=", $refused > 0 ? 1 : 0, "\n";
echo "waited=", $waited > 0 ? 1 : 0, "\n";
echo "fell_back=", $fellBack, "\n";
echo "done\n";
?>
--EXPECT--
status=200
len=393216
hash_match=1
refused=1
waited=1
fell_back=0
done
