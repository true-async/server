--TEST--
HttpResponse::write() — chunk framing holds either side of the coalescing threshold
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* A frame at or below H1_CHUNK_COALESCE_MAX leaves as one write, a larger one
 * as three, and the header block rides inside the first frame when it fits
 * (src/http1/http1_stream.c). The split is an optimisation and must not show
 * on the wire: this reads the raw response and checks the chunk-size lines
 * against the lengths the handler wrote, plus the de-chunked body byte for
 * byte.
 *
 * Three shapes, because each takes a different branch through the first
 * chunk: /small opens under the bound, so the headers travel in the frame;
 * /large opens with a 64 KiB chunk, so they go out on their own and the frame
 * follows as three writes; /empty opens with an empty chunk, which carries no
 * frame at all and must still put the headers on the wire. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port  = tas_free_port();

/* Per route: the chunk sizes the handler writes, in order. An empty chunk is
 * written as 0 and never reaches the wire — mark_ended owns the terminator. */
$plan = [
    '/small' => [1024, 32 * 1024, 32 * 1024 + 1, 64 * 1024],
    '/large' => [64 * 1024, 512],
    '/empty' => [0, 64],
];

$body = function (array $sizes): string {
    $out = '';
    foreach ($sizes as $i => $n) {
        $out .= str_repeat(chr(65 + $i), $n);
    }
    return $out;
};

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10));

$server->addHttpHandler(function ($req, $res) use ($plan, $server) {
    $sizes = $plan[$req->getPath()] ?? [];

    $res->setStatusCode(200)->setHeader('Content-Type', 'application/octet-stream');

    foreach ($sizes as $i => $n) {
        $res->write($n === 0 ? '' : str_repeat(chr(65 + $i), $n));
    }

    $res->end();

    if ($req->getPath() === '/empty') {
        $server->stop();
    }
});

$fetch = function (int $port, string $path): array {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    $wire = '';
    while (!feof($fp)) {
        $c = fread($fp, 65536);
        if ($c === '' || $c === false) break;
        $wire .= $c;
    }
    fclose($fp);

    return explode("\r\n\r\n", $wire, 2);
};

$cli = spawn(function () use ($port, $plan, $body, $fetch) {
    usleep(30000);
    foreach ($plan as $path => $sizes) {
    [$head, $rest] = $fetch($port, $path);
    $expected = $body($sizes);
    $sizes    = array_values(array_filter($sizes));
    echo "== $path\n";
    echo "chunked=", (int)(bool)preg_match('/^transfer-encoding:\s*chunked/mi', $head), "\n";

    /* Walk the chunked body by hand: every size line must match what the
     * handler wrote, in order, and the terminator must be the last thing. */
    $read  = '';
    $seen  = [];
    $off   = 0;
    while (true) {
        $eol = strpos($rest, "\r\n", $off);
        if ($eol === false) { echo "TRUNCATED\n"; break; }
        $len = hexdec(substr($rest, $off, $eol - $off));
        $off = $eol + 2;
        if ($len === 0) break;
        $seen[] = $len;
        $read  .= substr($rest, $off, $len);
        $off   += $len + 2;
    }

    echo "sizes_match=", (int)($seen === $sizes), "\n";
    echo "sizes=", implode(',', $seen), "\n";
    echo "body_len=", strlen($read), "\n";
    echo "body_match=", (int)($read === $expected), "\n";
    }
});

$server->start();
await($cli);
echo "done\n";
?>
--EXPECT--
== /small
chunked=1
sizes_match=1
sizes=1024,32768,32769,65536
body_len=132097
body_match=1
== /large
chunked=1
sizes_match=1
sizes=65536,512
body_len=66048
body_match=1
== /empty
chunked=1
sizes_match=1
sizes=64
body_len=64
body_match=1
done
