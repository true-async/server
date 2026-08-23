--TEST--
A handler killed by a bailout answers 500, not its half-built body
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--INI--
memory_limit=8M
display_errors=0
log_errors=0
--FILE--
<?php
/* The handler writes a status and part of a body, then exhausts memory_limit,
 * which is an E_ERROR and therefore a zend_bailout. The firewall in each
 * handler entry catches the longjmp and records it, but only HTTP/1 read that
 * record on the buffered path: HTTP/2 committed the response object as the
 * handler had left it, so the peer got a complete 200 over a body that stops
 * where the handler died.
 *
 * Both protocols must answer 500 here. The body is asserted too — a 500 whose
 * body is still 'half-built' would mean the status was rewritten over a body
 * that was not. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(5)
        ->setWriteTimeout(5)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setBody('half-built');
    $doomed = str_repeat('a', 64 * 1024 * 1024);
    $res->setBody('unreachable');
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $out   = tempnam(sys_get_temp_dir(), 'bailout');
    $lines = [];

    /* The firewall dumps a C backtrace to stderr per bailout, and run-tests
     * merges stderr into the compared output; collecting the answers and
     * printing them after the last request keeps them in one block. */
    foreach (['h1' => '--http1.1', 'h2' => '--http2-prior-knowledge'] as $tag => $opt) {
        $status = trim((string)shell_exec(sprintf(
            'curl -s %s --max-time 5 -o %s -w %%{http_code} http://127.0.0.1:%d/ 2>&1',
            $opt, escapeshellarg($out), $port)));

        $lines[] = "$tag status=$status body=" . file_get_contents($out);
    }

    @unlink($out);
    $server->stop();

    echo implode("\n", $lines), "\n";
});

$server->start();
await($client);
echo "Done\n";
--EXPECTF--
%Ah1 status=500 body=Internal Server Error
h2 status=500 body=Internal Server Error
Done
