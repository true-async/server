--TEST--
HTTP/2: a body that will not fit in memory is refused, and the connection keeps going
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['h2load' => true]);
?>
--INI--
memory_limit=8M
display_errors=0
--FILE--
<?php
/* cb_on_data_chunk_recv pre-sizes the body buffer from Content-Length in one
 * allocation, and under a memory_limit tighter than setMaxBodySize that
 * allocation bails out. The firewall catches the bailout so the connection
 * survives, and the stream was then meant to end in a reset — which never
 * reached the wire, because the refusal was spelled as
 * NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE and that callback's contract does not
 * carry it. Against the unfixed build twelve of these sixteen requests are
 * neither answered nor reset and h2load reports them as timeouts.
 *
 * Four of the sixteen fit in the limit and answer; which four is up to the
 * allocator, so the assertion is the one the fix owns: every request reaches
 * an end, and none of them waits for one. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

const REQUESTS  = 16;
const BODY_SIZE = 1024 * 1024;

$payload = sys_get_temp_dir() . '/tas-h2-oom-' . getmypid() . '.bin';
file_put_contents($payload, str_repeat('a', BODY_SIZE));

$port = tas_free_port();
$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10)
    ->setMaxBodySize(64 * 1024 * 1024)
    ->setMaxInflightRequests(64);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) {
    $req->awaitBody();
    $res->setStatusCode(200)->setBody('len=' . strlen($req->getBody()));
});

$client = spawn(function () use ($port, $server, $payload) {
    $out = (string)shell_exec(sprintf(
        'h2load -n %d -c 4 -m 4 -t 1 --connection-active-timeout=10 -d %s '
        . 'http://127.0.0.1:%d/ 2>&1',
        REQUESTS, escapeshellarg($payload), $port));

    $done = -1; $timeout = -1;
    if (preg_match('/(\d+) done,/', $out, $m))     { $done    = (int)$m[1]; }
    if (preg_match('/(\d+) timeout/', $out, $m))   { $timeout = (int)$m[1]; }

    echo "done=$done of ", REQUESTS, "\n";
    echo "timeout=$timeout\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($payload);
echo "finished\n";
--EXPECT--
done=16 of 16
timeout=0
finished
