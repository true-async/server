--TEST--
HttpResponse body API — write() streams, appendBody() buffers, and the two modes refuse to mix
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* The renames of #180, checked from PHP where an adapter would meet them.
 *
 * Two claims about the modes. write() streams: the response is chunked, so no
 * Content-Length is computed and a setHeader() after the first chunk throws.
 * appendBody() buffers: nothing leaves before end(), and the header is still
 * writable in between.
 *
 * The removed names are asserted absent: send(), isClosed(), getBodyStream()
 * and setBodyStream() are undefined methods, so an old handler fails at the
 * call rather than somewhere downstream. sendable() is the one exception — it
 * keeps its declaration and throws, because its two replacements are not
 * guessable from the name; h2/023 asserts that on a live stream.
 *
 * /mixed guards the fourth claim. The streaming path never reads the buffered
 * body, so streaming on top of one dropped those bytes with no error at all —
 * the client saw the streamed chunk alone. Both directions throw now, and the
 * buffered body still reaches the wire after the refusal. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)->setWriteTimeout(5));

$probe = [];
$server->addHttpHandler(function ($req, $res) use (&$probe, $server) {
    $path = $req->getPath();

    if ($path === '/buffered') {
        $res->appendBody('one ');
        /* Buffered appending commits nothing, so headers stay open. */
        $res->setHeader('X-After-Append', 'yes');
        $res->appendBody('two');
        $probe['buffered_headers_sent'] = $res->isHeadersSent();
        $probe['buffered_body']         = $res->getBody();
        $res->end();
        return;
    }

    if ($path === '/mixed') {
        $res->appendBody('buffered');
        try {
            $res->write('streamed');
            $probe['write_after_append'] = 'NO-THROW';
        } catch (\Throwable $e) {
            $probe['write_after_append'] = get_class($e);
        }
        $res->end();
        return;
    }

    if ($path === '/two-chunks') {
        $res->write('one-');
        $probe['stream_headers_sent_early'] = $res->isHeadersSent();
        $res->write('two');
        $res->end();
        $probe['stream_ended'] = $res->isEnded();
        return;
    }

    $res->write('streamed');
    $probe['stream_headers_sent'] = $res->isHeadersSent();
    try {
        $res->setHeader('X-Too-Late', 'yes');
        $probe['header_after_write'] = 'NO-THROW';
    } catch (\Throwable $e) {
        $probe['header_after_write'] = get_class($e);
    }
    $res->end();
    $server->stop();
});

$get = function (int $port, string $path): string {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    $buf = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $buf .= $c;
    }
    fclose($fp);
    return preg_replace("/^Date: [^\r\n]*\r?\n/mi", "", $buf);
};

$cli = spawn(function () use ($port, $get) {
    usleep(30000);
    foreach (['/buffered', '/mixed', '/two-chunks', '/streamed'] as $path) {
        $wire = $get($port, $path);
        [$head, $body] = explode("\r\n\r\n", $wire, 2);
        echo "== $path\n";
        echo "content_length=", (preg_match('/^content-length:\s*(\d+)/mi', $head, $m) ? $m[1] : 'none'), "\n";
        echo "chunked=", (int)(bool)preg_match('/^transfer-encoding:\s*chunked/mi', $head), "\n";
        echo "after_append_header=", (int)(bool)preg_match('/^x-after-append:/mi', $head), "\n";
        echo "body=", trim(preg_replace('/^[0-9a-f]+\r\n|\r\n0\r\n\r\n$|\r\n/mi', '', $body)), "\n";
    }
});

$server->start();
await($cli);

echo "== removed\n";
echo "send=", (int)method_exists('TrueAsync\\HttpResponse', 'send'), "\n";
echo "getBodyStream=", (int)method_exists('TrueAsync\\HttpResponse', 'getBodyStream'), "\n";
echo "setBodyStream=", (int)method_exists('TrueAsync\\HttpResponse', 'setBodyStream'), "\n";
echo "isClosed=", (int)method_exists('TrueAsync\\HttpResponse', 'isClosed'), "\n";
echo "sendable=", (int)method_exists('TrueAsync\\HttpResponse', 'sendable'), "\n";

echo "== probe\n";
foreach ($probe as $k => $v) echo "$k = " . var_export($v, true) . "\n";
?>
--EXPECT--
== /buffered
content_length=7
chunked=0
after_append_header=1
body=one two
== /mixed
content_length=8
chunked=0
after_append_header=0
body=buffered
== /two-chunks
content_length=none
chunked=1
after_append_header=0
body=one-two
== /streamed
content_length=none
chunked=1
after_append_header=0
body=streamed
== removed
send=0
getBodyStream=0
setBodyStream=0
isClosed=0
sendable=1
== probe
buffered_headers_sent = false
buffered_body = 'one two'
write_after_append = 'TrueAsync\\HttpServerRuntimeException'
stream_headers_sent_early = true
stream_ended = true
stream_headers_sent = true
header_after_write = 'TrueAsync\\HttpServerRuntimeException'
