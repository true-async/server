--TEST--
HTTP/1 chunked stream — edge cases: empty chunks, end() with no send, large chunk
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19340 + getmypid() % 100;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10));

$count = 0;
$server->addHttpHandler(function ($req, $res) use (&$count, $server) {
    $path = $req->getUri();
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');

    if ($path === '/empty-chunks') {
        // Empty chunk should be silently dropped (not emit zero-chunk EOF).
        $res->send("real1\n");
        $res->send("");           // dropped
        $res->send("");           // dropped
        $res->send("real2\n");
    } elseif ($path === '/no-send') {
        // No send() call. end() must still commit headers + zero chunk
        // (covers h1_stream_mark_ended's "headers-not-sent" branch).
    } elseif ($path === '/large') {
        // 8 KB chunk — exercises the hex header path beyond a few digits.
        $res->send(str_repeat('A', 8192));
    } else {
        $res->setStatusCode(404)->setBody('nf');
    }
    $res->end();
    if (++$count >= 3) $server->stop();
});

$cli = spawn(function () use ($port) {
    usleep(30000);
    foreach (['/empty-chunks', '/no-send', '/large'] as $path) {
        $cmd = sprintf('curl --http1.1 -i -s --max-time 3 http://127.0.0.1:%d%s 2>&1', $port, $path);
        $out = shell_exec($cmd);
        echo "=== $path ===\n";

        // Print TE header presence + body length only (avoid wire variability)
        $has_chunked = (bool)preg_match('/^Transfer-Encoding:\s*chunked/mi', $out);
        echo "te_chunked=" . ($has_chunked ? 1 : 0) . "\n";

        // Body lives after the blank line
        $parts = preg_split("/\r?\n\r?\n/", $out, 2);
        $body = $parts[1] ?? '';
        echo "body_len=" . strlen($body) . "\n";

        if ($path === '/empty-chunks') {
            // Should contain both real1 and real2 (empties dropped)
            echo "has_real1=" . (str_contains($body, 'real1') ? 1 : 0) . "\n";
            echo "has_real2=" . (str_contains($body, 'real2') ? 1 : 0) . "\n";
        } elseif ($path === '/large') {
            // Body (after dechunking) should still contain 8192 'A's
            echo "has_8kb=" . (substr_count($body, 'A') === 8192 ? 1 : 0) . "\n";
        }
    }
});

$server->start();
await($cli);
--EXPECT--
=== /empty-chunks ===
te_chunked=1
body_len=12
has_real1=1
has_real2=1
=== /no-send ===
te_chunked=0
body_len=0
=== /large ===
te_chunked=1
body_len=8192
has_8kb=1
