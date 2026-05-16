--TEST--
HttpRequest::readBody() — H1 streaming request body (issue #26)
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

$port = 19920 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setBodyStreamingEnabled(true)
        ->setMaxBodySize(8 * 1024 * 1024)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $total  = 0;
    $chunks = 0;
    while (($c = $req->readBody()) !== null) {
        $total  += strlen($c);
        $chunks++;
    }
    // Second readBody() after EOF must be null (idempotent).
    $tail = $req->readBody();
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("bytes=$total tail=" . ($tail === null ? 'null' : strlen($tail)));
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);
    // 64 KiB body — large enough to require multiple on_body callbacks.
    $body = str_repeat('A', 64 * 1024);
    $cmd  = sprintf(
        'curl --http1.1 -s --max-time 5 --data-binary @- -H "Expect:" http://127.0.0.1:%d/upload',
        $port
    );
    $resp = shell_exec("echo -n '$body' | $cmd");
    echo $resp, "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
bytes=65536 tail=null
done
