--TEST--
HttpServer: peer RST_STREAM cancels in-flight HTTP/2 handler with HttpException(499)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* Step 7.2a — peer-initiated stream reset must surface in the handler
 * coroutine as HttpException(code=499, "stream reset by peer"), NOT as
 * silent teardown. Verifies that (a) cancel fires with a live coroutine,
 * (b) try/catch{} blocks run, (c) refcount keeps the stream alive long
 * enough for dispose to unwind safely, (d) the server survives and the
 * next connection still works. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\HttpException;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19830 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10);

$server = new HttpServer($config);

$caught_code      = 0;
$caught_msg       = '';
$handler_finished = false;

$server->addHttpHandler(function ($req, $res)
    use (&$caught_code, &$caught_msg, &$handler_finished) {
    if ($req->getUri() !== '/slow') {
        /* Fast path for the post-cancel liveness probe. */
        $res->setStatusCode(200)->setBody('ok');
        return;
    }
    try {
        /* curl below aborts after ~250 ms; delay is long enough that
         * cancel always races in before natural completion. */
        delay(3000);
        $handler_finished = true;
        $res->setStatusCode(200)->setBody('unexpected');
    } catch (HttpException $e) {
        $caught_code = $e->getCode();
        $caught_msg  = $e->getMessage();
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http2-prior-knowledge -s -o /dev/null --max-time 0.25 '
        . 'http://127.0.0.1:%d/slow',
        $port
    );
    exec($cmd, $out, $rc);
    /* curl --max-time returns rc=28 on timeout. */
    echo "curl_rc_is_timeout=", (int)($rc === 28), "\n";

    /* Give the server a tick to process RST / connection close and
     * drive the cancel + dispose through the scheduler. */
    delay(200);

    /* Next request on a fresh connection must still work — proves the
     * cancel path didn't damage server state. */
    $cmd2 = sprintf(
        'curl --http2-prior-knowledge -s --max-time 2 http://127.0.0.1:%d/ok',
        $port
    );
    $out2 = [];
    exec($cmd2, $out2, $rc2);
    echo "next_req_rc=$rc2\n";

    $server->stop();
});

$server->start();
await($client);

echo "handler_finished=", (int)$handler_finished, "\n";
echo "caught_code=$caught_code\n";
echo "caught_msg=$caught_msg\n";
echo "done\n";
--EXPECT--
curl_rc_is_timeout=1
next_req_rc=0
handler_finished=0
caught_code=499
caught_msg=stream reset by peer
done
