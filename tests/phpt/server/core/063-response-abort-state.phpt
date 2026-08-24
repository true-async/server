--TEST--
HttpResponse::abort() — what every other method answers once a stream is failed
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
/* abort() is meant to be called from a catch or a finally, where the handler
 * is already carrying an error. That is what shapes the edges asserted here:
 * a second abort() and an abort() with nothing to disown both stay silent,
 * because a method that throws from there replaces the handler's diagnosis
 * with its own. The one refusal left is abort() after end(), where the client
 * has already been told the body is whole and no later call can take it back.
 *
 * Everything that would put bytes on a failed stream throws instead, and the
 * two predicates agree with each other: the response is ended, and it is not
 * writable.
 *
 * The three shapes a started stream can be in when abort() reaches it are all
 * here, because they differ only in what the transport can still do: bytes on
 * the wire (/aborted), nothing on the wire (/sse-no-event), and a body already
 * finished (/ended). Which call finished the response is the same answer in
 * all three, and it is the answer every refusal below quotes. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(5)
);

$log = [];

function why(callable $fn): string
{
    try {
        $fn();
        return 'no throw';
    } catch (\Throwable $e) {
        return $e->getMessage();
    }
}

$server->addHttpHandler(function ($req, $res) use (&$log) {
    switch ($req->getUri()) {
        case '/aborted':
            $res->write('half');
            $res->abort();

            $log[] = 'ended='    . (int) $res->isEnded();
            $log[] = 'writable=' . (int) $res->isWritable();
            $log[] = 'second abort: ' . why(fn () => $res->abort());
            $log[] = 'write: '        . why(fn () => $res->write('more'));
            $log[] = 'tryWrite: '     . why(fn () => $res->tryWrite('more'));
            $log[] = 'sseEvent: '     . why(fn () => $res->sseEvent('more'));
            $log[] = 'end: '          . why(fn () => $res->end());
            $log[] = 'setTrailer: '   . why(fn () => $res->setTrailer('x-a', 'b'));
            $log[] = 'setHeader: '    . why(fn () => $res->setHeader('x-a', 'b'));
            break;

        case '/bad-code':
            $res->write('half');
            $log[] = 'negative: '     . why(fn () => $res->abort(-1));
            $log[] = 'too wide: '     . why(fn () => $res->abort(0x100000000));
            $log[] = 'top of range: ' . why(fn () => $res->abort(0xFFFFFFFF));
            break;

        case '/ended':
            $res->write('whole');
            $res->end();
            $log[] = 'abort after end: ' . why(fn () => $res->abort());
            break;

        case '/sse-no-event':
            /* Streaming, and nothing on the wire: sseStart() commits its
             * headers lazily, so the transport has no half body to fail and
             * the peer gets a whole empty response. abort() still finished
             * the response, so the refusals name it. */
            $res->sseStart();
            $res->abort();

            $log[] = 'sse ended='    . (int) $res->isEnded();
            $log[] = 'sse second abort: ' . why(fn () => $res->abort());
            $log[] = 'sse write: '        . why(fn () => $res->write('more'));
            $log[] = 'sse end: '          . why(fn () => $res->end());
            break;

        case '/never-started':
            /* Nothing was streamed, so there is nothing to disown: abort()
             * leaves the response alone and end() still answers normally. */
            $log[] = 'abort unstarted: ' . why(fn () => $res->abort());
            $log[] = 'ended='  . (int) $res->isEnded();
            $res->end('buffered');
            break;
    }
});

spawn(function () use ($port, $server) {
    usleep(50000);

    foreach (['/aborted', '/ended', '/sse-no-event', '/never-started', '/bad-code'] as $path) {
        $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET $path HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

        while (!feof($fp)) {
            if (fread($fp, 8192) === false) {
                break;
            }
        }

        fclose($fp);
    }

    $server->stop();
});

$server->start();

echo implode("\n", $log), "\n";
?>
--EXPECT--
ended=1
writable=0
second abort: no throw
write: Response already closed — cannot write() after abort()
tryWrite: Response already closed — cannot tryWrite() after abort()
sseEvent: Cannot sseEvent() after abort()
end: Response has already been aborted
setTrailer: Cannot set trailers after abort() has been called
setHeader: Cannot modify response after abort() has been called
abort after end: Response has already been closed — the body was already finished cleanly
sse ended=1
sse second abort: no throw
sse write: Response already closed — cannot write() after abort()
sse end: Response has already been aborted
abort unstarted: no throw
ended=0
negative: abort(): error code must be between 0 and 4294967295
too wide: abort(): error code must be between 0 and 4294967295
top of range: no throw
