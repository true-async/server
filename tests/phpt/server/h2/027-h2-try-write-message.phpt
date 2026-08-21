--TEST--
HttpResponse::tryWriteMessage() — false when the ring is full, and the frame matches writeMessage()
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The gRPC dialect of tryWrite(). A refused message queued nothing, so the
 * same message may be offered again after awaitWritable(); the byte-exact body
 * proves it, because a queued-and-retried message would appear twice. The
 * messages alternate between the two twins, so the same body also proves they
 * frame identically — five bytes of prefix, compressed flag then length, which
 * is what makes this a dialect rather than a wrapper around write(). */

require_once __DIR__ . '/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$MSG_SZ = 8192;
$N_MSGS = 48;

$expected = '';
for ($i = 0; $i < $N_MSGS; $i++) {
    $payload = str_repeat(chr(33 + ($i % 90)), $MSG_SZ);
    $expected .= "\x00" . pack('N', $MSG_SZ) . $payload;
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

$server->addHttpHandler(function ($req, $res) use ($MSG_SZ, $N_MSGS, &$refused, &$waited, &$fellBack) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'application/grpc');

    for ($i = 0; $i < $N_MSGS; $i++) {
        $payload = str_repeat(chr(33 + ($i % 90)), $MSG_SZ);

        /* Every other message goes out through the blocking twin, so the
         * byte-exact body compares the two framings against each other rather
         * than each against the same expectation. */
        if ($i % 2 === 1) {
            $res->writeMessage($payload);
            continue;
        }

        if (!$res->tryWriteMessage($payload)) {
            $refused++;

            if ($res->awaitWritable(5000)) {
                $waited++;
            }

            if (!$res->tryWriteMessage($payload)) {
                $fellBack++;
                $res->writeMessage($payload);
            }
        }
    }

    $res->end();
});

$client = spawn(function () use ($port, $server, $expected) {
    usleep(50000);
    try {
        $cli = new H2TestClient('127.0.0.1', $port, 15);
        $sid = $cli->sendRequest('GET', '/grpc', "127.0.0.1:$port");
        [$status, $body, $trailers, $ended] = $cli->collectResponse($sid, true);
        $cli->close();

        echo "status=$status\n";
        echo 'len=', strlen($body), "\n";
        echo 'hash_match=', (sha1($body) === sha1($expected) ? 1 : 0), "\n";
        echo 'first_frame_prefix=', bin2hex(substr($body, 0, 5)), "\n";
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
len=393456
hash_match=1
first_frame_prefix=0000002000
refused=1
waited=1
fell_back=0
done
