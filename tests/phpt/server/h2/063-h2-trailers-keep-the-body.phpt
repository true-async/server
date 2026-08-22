--TEST--
HTTP/2 — a response carrying trailers still delivers its body
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
if (!in_array('gzip', TrueAsync\HttpServerConfig::getSupportedEncodings(), true)) {
    die('skip gzip backend not built');
}
?>
--FILE--
<?php
/* nghttp2 takes trailers only from inside the data provider, at true EOF, with
 * NO_END_STREAM on the last DATA slice (`nghttp2_submit_trailer`, nghttp2.h).
 * The buffered path used to queue them straight after the response instead, so
 * the DATA that had not left yet was displaced: a 200 with the trailer block
 * and an empty body, which is a truncation the framing calls complete. The
 * streaming path already submitted at EOF and is the control here.
 *
 * The empty body is the other half of the same rule: with no DATA frame at all
 * the HEADERS carries END_STREAM, and a trailer block cannot follow it, so the
 * trailers were dropped without a trace. Such a response now takes a
 * zero-length DATA frame to hang them off.
 *
 * The compressed shape is here because the codec replaces the body after the
 * handler returns and the response is submitted through a wrapper, so it is a
 * different route to the same data provider: 4096 bytes decoded to nothing. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
        ->setCompressionEnabled(true)
);

$bulk = str_repeat('abcdefgh', 512);   /* 4096 bytes, past the compression floor */

$server->addHttpHandler(function ($req, $res) use ($bulk) {
    switch ($req->getPath()) {
        case '/buffered':
            $res->setTrailer('x-done', '1')->setStatusCode(200)->setBody('payload');
            break;

        case '/empty':
            $res->setTrailer('x-done', '1')->setStatusCode(200)->setBody('');
            break;

        case '/gzipped':
            $res->setTrailer('x-done', '1')->setStatusCode(200)
                ->setHeader('Content-Type', 'text/plain')->setBody($bulk);
            break;

        default:
            $res->setStatusCode(200)->setTrailer('x-done', '1');
            $res->write('pay');
            $res->write('load');
            $res->end();
    }
});

spawn(function () use ($port, $server) {
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);

        if ($fp) { fclose($fp); break; }

        usleep(20000);
    }

    foreach (['/buffered', '/empty', '/gzipped', '/streamed'] as $path) {
        $out = [];
        exec(sprintf(
            'curl --http2-prior-knowledge -s -v --max-time 3 http://127.0.0.1:%d%s 2>&1',
            $port, $path), $out);
        $blob = implode("\n", $out);

        /* Counted off the decoded stream rather than curl's %{size_download},
         * which reports the octets on the wire and so reads 49 for the gzipped
         * body. What the assertion is about is the body the handler wrote. */
        $body = (string) shell_exec(sprintf(
            'curl --http2-prior-knowledge --compressed -s --max-time 3 '
            . 'http://127.0.0.1:%d%s 2>/dev/null | wc -c',
            $port, $path));

        printf("%s body=%s trailer=%d\n",
               $path, trim($body), (int) (strpos($blob, 'x-done: 1') !== false));
    }

    $server->stop();
});

$server->start();
?>
--EXPECT--
/buffered body=7 trailer=1
/empty body=0 trailer=1
/gzipped body=4096 trailer=1
/streamed body=7 trailer=1
