--TEST--
HttpResponse::send() — streaming exercises backpressure wake via WINDOW_UPDATE
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Step 5b Phase 1.1 — the critical test that proves the suspend+
 * wake-on-WINDOW_UPDATE path actually works. Server sends 256 KiB
 * in 32 KiB chunks; that's 4× the default 64 KiB stream initial
 * window. Handler must suspend in send() when drain stalls, then
 * wake each time our client sends WINDOW_UPDATE. Byte-exact hash
 * verifies nothing was lost.
 *
 * curl ≥ 8.12 has a quirk where it doesn't spontaneously emit
 * WINDOW_UPDATE after receiving the initial window's worth of
 * bytes (see memory: project_step5_phase1_phpt_quirks), so this
 * test uses a pure-PHP H2 client (_h2_client.inc) that sends
 * WINDOW_UPDATE explicitly. */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19870 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(15)
    ->setWriteTimeout(15);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');
    /* 32 KiB × 8 = 256 KiB. Well past the 64 KiB initial window
     * — forces the suspend loop in h2_stream_append_chunk. */
    $chunk = str_repeat('A', 32768);
    for ($i = 0; $i < 8; $i++) {
        $res->send($chunk);
    }
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);
    try {
        $cli = new H2TestClient('127.0.0.1', $port, 10);
        $sid = $cli->sendRequest('GET', '/stream', "127.0.0.1:$port");
        [$status, $body, $trailers, $ended] = $cli->collectResponse($sid, true);
        $cli->close();

        echo "status=$status\n";
        echo "len=", strlen($body), "\n";
        echo "ended=", (int)$ended, "\n";
        echo "hash_match=", (sha1($body) === sha1(str_repeat('A', 8 * 32768)) ? 1 : 0), "\n";
    } catch (\Throwable $e) {
        echo "ERR: ", $e->getMessage(), "\n";
    }
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
status=200
len=262144
ended=1
hash_match=1
done
