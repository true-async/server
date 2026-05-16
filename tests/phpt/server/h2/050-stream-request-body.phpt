--TEST--
HttpRequest::readBody() — H2 streaming request body (issue #26)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
if (strpos((string)shell_exec('curl --version 2>&1'), 'HTTP2') === false) die('skip curl built without HTTP/2');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19950 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setBodyStreamingEnabled(true)
        ->setMaxBodySize(8 * 1024 * 1024)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $total = 0;
    while (($c = $req->readBody()) !== null) {
        $total += strlen($c);
    }
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/plain')
        ->setBody("bytes=$total");
});

$client = spawn(function () use ($port, $server) {
    usleep(50000);
    $body = str_repeat('A', 256 * 1024);  // 256 KiB
    $tmp  = tempnam(sys_get_temp_dir(), 'h2body');
    file_put_contents($tmp, $body);
    $cmd  = sprintf(
        'curl --http2-prior-knowledge -s --max-time 5 --data-binary @%s -H "Expect:" http://127.0.0.1:%d/upload',
        $tmp, $port
    );
    $resp = shell_exec($cmd);
    @unlink($tmp);
    echo $resp, "\n";
    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
bytes=262144
done
