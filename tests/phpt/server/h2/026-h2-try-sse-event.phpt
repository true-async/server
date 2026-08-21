--TEST--
HttpResponse::trySseEvent() — false when the ring is full, and a refusal queues nothing
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The SSE dialect of tryWrite(): a refused event queued nothing, so the same
 * event may be offered again after awaitWritable(). The byte-exact body is
 * what proves it — had the refused record been queued too, it would appear
 * twice. The single-threaded scheduler runs the handler until it suspends, so
 * the client cannot credit the window meanwhile and a refusal is guaranteed. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$DATA_SZ = 8192;
$N_EVENTS = 48;

$expected = '';
for ($i = 0; $i < $N_EVENTS; $i++) {
    $expected .= 'data: ' . str_repeat(chr(33 + ($i % 90)), $DATA_SZ) . "\n\n";
}

$refused = 0;
$waited = 0;
$fellBack = 0;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(15)
        ->setWriteTimeout(15)
);

$server->addHttpHandler(function ($req, $res) use ($DATA_SZ, $N_EVENTS, &$refused, &$waited, &$fellBack) {
    $res->sseStart();

    for ($i = 0; $i < $N_EVENTS; $i++) {
        $data = str_repeat(chr(33 + ($i % 90)), $DATA_SZ);

        if (!$res->trySseEvent($data)) {
            $refused++;

            if ($res->awaitWritable(5000)) {
                $waited++;
            }

            if (!$res->trySseEvent($data)) {
                $fellBack++;
                $res->sseEvent($data);
            }
        }
    }

    $res->end();
});

$client = spawn(function () use ($port, $server, $expected) {
    usleep(50000);
    try {
        $cli = new H2TestClient('127.0.0.1', $port, 15);
        $sid = $cli->sendRequest('GET', '/events', "127.0.0.1:$port");
        [$status, $body, $trailers, $ended] = $cli->collectResponse($sid, true);
        $cli->close();

        echo "status=$status\n";
        echo 'len=', strlen($body), "\n";
        echo 'hash_match=', (sha1($body) === sha1($expected) ? 1 : 0), "\n";
    } catch (\Throwable $e) {
        echo 'ERR: ', $e->getMessage(), "\n";
    }
    $server->stop();
});

$server->start();
await($client);

echo 'refused=', $refused > 0 ? 1 : 0, "\n";
echo 'waited=', $waited > 0 ? 1 : 0, "\n";
echo 'fell_back=', $fellBack, "\n";
echo "done\n";
?>
--EXPECT--
status=200
len=393600
hash_match=1
refused=1
waited=1
fell_back=0
done
