--TEST--
A compressed stream is finished at dispose after the peer has gone
--EXTENSIONS--
true_async_server
true_async
sockets
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif([]);
if (!extension_loaded('zlib')) die('skip zlib required');
?>
--FILE--
<?php
/* The codec trailer is written from the dispose path, which runs inside the
 * coroutine's own finalize — past the point where a cancellation could reach
 * it. Anything that waits there has to end on its own, and the peer here is
 * gone, so the shape worth guarding is a handler that fills the outbound ring,
 * loses its client and then falls out without end().
 *
 * The assertion is the plainest one there is: the script reaches its last
 * line. That covers the whole dispose-through-the-wrapper route under a
 * departed peer — a wait that never returned would hang the runner instead.
 *
 * It does not reach the wait itself: the ring drains as the handler fills it,
 * so the trailer finds room. Holding the window shut needs a peer that stays
 * alive, which is the other half of the same condition, and no shape reached
 * from PHP puts the two together. The guard for that lives in the wait loop,
 * which refuses to park on a peer that has gone. */

require_once __DIR__ . '/../h2/_h2_client.inc';

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$reached_end = false;

$server->addHttpHandler(function ($req, $res) use (&$reached_end) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');

    /* Past the 256 KiB default byte cap, so the ring stays over it. */
    for ($i = 0; $i < 6; $i++) {
        $res->write(str_repeat('x', 65536));
    }

    $reached_end = true;
    /* No end(): the dispose path writes the codec trailer. */
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    $cli = new H2TestClient('127.0.0.1', $port, 10);
    $cli->sendRequest('GET', '/stream', "127.0.0.1:$port",
                      ['accept-encoding' => 'gzip']);

    /* Let the handler fill the ring, then leave without draining it. */
    delay(300);
    $cli->close();

    delay(300);
    $server->stop();
});

$server->start();
await($client);

echo "handler reached its end: ", (int) $reached_end, "\n";
echo "done\n";
?>
--EXPECT--
handler reached its end: 1
done
