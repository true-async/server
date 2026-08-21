--TEST--
HttpResponse::trySseEvent() — HTTP/1 never refuses, and the record matches sseEvent()
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
/* The non-blocking twin writes the same record as the blocking one. HTTP/1
 * keeps no queue of its own, so every offer is accepted and the answer is
 * always true — the refusal path is exercised over HTTP/2, where a ring
 * exists (h2/026). Both routes emit the same three events, and the test
 * compares the two bodies byte for byte. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$port = tas_free_port();
$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$answers = [];

$server->addHttpHandler(function ($req, $res) use (&$answers) {
    $blocking = $req->getPath() === '/blocking';

    $res->sseStart();

    foreach ([['hello', null, null], ['line1' . "\n" . 'line2', null, null], ['tick', 'ping', '42']] as [$data, $event, $id]) {
        if ($blocking) {
            $res->sseEvent($data, event: $event, id: $id);
        } else {
            $answers[] = $res->trySseEvent($data, event: $event, id: $id) ? 'true' : 'false';
        }
    }

    /* Every argument unset asks the queue for nothing, so it cannot be a
     * refusal. */
    if (!$blocking) {
        $answers[] = $res->trySseEvent() ? 'true' : 'false';
    }

    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    $fetch = static function (string $path) use ($port): string {
        $cmd = sprintf('curl --http1.1 -s -N --max-time 3 http://127.0.0.1:%d%s', $port, $path);
        return (string) shell_exec($cmd);
    };

    $blocking = $fetch('/blocking');
    $nonblocking = $fetch('/nonblocking');

    echo 'bodies_match=', ($blocking === $nonblocking ? 1 : 0), "\n";
    echo 'body=', str_replace("\n", '\n', $nonblocking), "\n";

    $server->stop();
});

$server->start();
await($client);

echo 'answers=', implode(',', $answers), "\n";
echo "done\n";
?>
--EXPECT--
bodies_match=1
body=data: hello\n\ndata: line1\ndata: line2\n\nid: 42\nevent: ping\ndata: tick\n\n
answers=true,true,true,true
done
