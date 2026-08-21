--TEST--
HttpServer: a drain that comes due after the header block still retires the connection
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A streamed response decides how its connection ends where it builds its
 * header block, because that is the only place the peer can be told. That
 * decision holds for the instant it was taken, and a long stream outlives it:
 * a connection can reach its age limit while its body is still going out.
 *
 * The peer cannot be told then — its header block left long ago — but the
 * connection still has to retire, or an ageing socket that happens to be
 * streaming is never retired at all.
 *
 * One connection, an age of one second and a handler that streams for longer.
 * What is read afterwards is the socket rather than a header: the response is
 * asserted to carry no `Connection: close`, because by then it could not, and
 * the proof is the request sent on that socket after it, which goes
 * unanswered. The pause before that request is what the assertion rests on —
 * a request already in the read buffer when the response ends is answered
 * whatever the connection's verdict, so the socket is given its finalisation
 * first. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setKeepAliveTimeout(30)
    ->setMaxConnectionAgeMs(1000)
    ->setMaxConnectionAgeGraceMs(30000)
    ->setDrainSpreadMs(100)
    ->setDrainCooldownMs(1000);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    /* The header block goes out here, while the connection is young. */
    $res->write('first');
    delay(1600);
    $res->write('second');
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $e, $es, 3);
    stream_set_timeout($fp, 8);
    fwrite($fp, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");

    $streamed = '';

    while (!feof($fp) && !str_contains($streamed, "0\r\n\r\n")) {
        $chunk = fread($fp, 4096);

        if ($chunk === false || $chunk === '') { break; }

        $streamed .= $chunk;
    }

    echo "streamed body: ",
        (str_contains($streamed, "5\r\nfirst\r\n6\r\nsecond\r\n0\r\n\r\n") ? 1 : 0), "\n";
    echo "close header: ", (preg_match('/^connection:\s*close/mi', $streamed) ? 1 : 0), "\n";

    usleep(100000);

    /* The socket the drain retired. A server that kept it answers this. */
    fwrite($fp, "GET /c HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    $after = '';

    while (!feof($fp)) {
        $chunk = fread($fp, 4096);

        if ($chunk === false || $chunk === '') { break; }

        $after .= $chunk;
    }

    echo "answered after drain: ", ($after === '' ? 0 : 1), "\n";

    fclose($fp);
    $server->stop();
});

$server->start();
await($client);
?>
--EXPECT--
streamed body: 1
close header: 0
answered after drain: 0
