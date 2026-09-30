--TEST--
HTTP/1: pipelined requests past the read buffer are all answered, in order, while a handler is busy
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* Behind a request whose handler is still running, pipelined requests wait in
 * the connection's 8 KiB read buffer. Filling it used to close the connection:
 * the allocator had no room to hand libuv, and the read failed with
 * UV_ENOBUFS, so only the requests inside the first 8 KiB were answered (104
 * of 151 below). The read now stops while the buffer is full and starts again
 * as the handlers drain it (#341). Three shapes: 150 GETs ending with
 * Connection: close; the same with the client's write half shut after the
 * pipeline; and 3000 GETs, about 30 times the buffer, with bodies on some. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addHttpHandler(function ($req, $res) {
    if ($req->getPath() === '/slow') {
        delay(300);
    }

    $res->setStatusCode(200)->setBody($req->getPath() . ':' . strlen($req->getBody()))->end();
});

/* One response at a time off the wire: headers, then Content-Length bytes. */
function read_responses($c): array
{
    $bodies = [];
    $buf = '';

    for (;;) {
        $head_end = strpos($buf, "\r\n\r\n");

        if ($head_end !== false && preg_match('/Content-Length: (\d+)/i', substr($buf, 0, $head_end), $m)
            && strlen($buf) >= $head_end + 4 + (int) $m[1]) {
            $bodies[] = substr($buf, $head_end + 4, (int) $m[1]);
            $buf = substr($buf, $head_end + 4 + (int) $m[1]);
            continue;
        }

        $chunk = fread($c, 65536);

        if ($chunk === false || $chunk === '') {
            return $bodies;
        }

        $buf .= $chunk;
    }
}

function run(int $port, string $wire, bool $shut_write): array
{
    $c = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($c, 10);

    $writer = spawn(function () use ($c, $wire, $shut_write) {
        $written = fwrite($c, $wire);

        if ($shut_write) {
            stream_socket_shutdown($c, STREAM_SHUT_WR);
        }

        return $written;
    });

    $bodies = read_responses($c);
    $written = await($writer);
    fclose($c);

    return [$written, $bodies];
}

function pipeline(int $n, int $body_every): array
{
    $wire = "GET /slow HTTP/1.1\r\nHost: t\r\n\r\n";
    $expected = ['/slow:0'];

    for ($i = 0; $i < $n; $i++) {
        $last = $i === $n - 1 ? "Connection: close\r\n" : '';
        $body = $body_every > 0 && $i % $body_every === 0 ? str_repeat('b', 300) : '';
        $method = $body === '' ? 'GET' : 'POST';
        $length = $body === '' ? '' : "Content-Length: " . strlen($body) . "\r\n";
        $wire .= "$method /r$i HTTP/1.1\r\nHost: t\r\nX-Pad: " . str_repeat('x', 40) . "\r\n$last$length\r\n$body";
        $expected[] = "/r$i:" . strlen($body);
    }

    return [$wire, $expected];
}

$client = spawn(function () use ($port, $server) {
    foreach ([
        ['150 GETs', 150, 0, false],
        ['150 GETs, write half shut', 150, 0, true],
        ['3000 requests with bodies', 3000, 7, false],
    ] as [$label, $n, $body_every, $shut]) {
        [$wire, $expected] = pipeline($n, $body_every);
        [$written, $bodies] = run($port, $wire, $shut);

        echo "$label: ", strlen($wire) > 8192 ? 'past' : 'within', " the buffer, written ",
            $written === strlen($wire) ? 'whole' : "$written of " . strlen($wire),
            ", answered ", count($bodies), " of ", count($expected),
            $bodies === $expected ? ', in order' : ', OUT OF ORDER OR MISSING', "\n";
    }

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
150 GETs: past the buffer, written whole, answered 151 of 151, in order
150 GETs, write half shut: past the buffer, written whole, answered 151 of 151, in order
3000 requests with bodies: past the buffer, written whole, answered 3001 of 3001, in order
done
