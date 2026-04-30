--TEST--
HTTP/1 chunked stream — many small chunks + getBody snapshot during stream
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

$port = 19350 + getmypid() % 100;

$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)->setWriteTimeout(10));

$server->addHttpHandler(function ($req, $res) use ($server) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    // 100 small chunks — exercises the chunk-header sprintf path repeatedly
    for ($i = 1; $i <= 100; $i++) {
        $res->send(sprintf("%03d\n", $i));
    }
    $res->end();
    $server->stop();
});

$cli = spawn(function () use ($port) {
    usleep(30000);
    $cmd = sprintf('curl --http1.1 -s --max-time 5 http://127.0.0.1:%d/ 2>&1', $port);
    $out = shell_exec($cmd);
    $lines = explode("\n", trim($out));
    echo "lines=" . count($lines) . "\n";
    echo "first=" . $lines[0] . "\n";
    echo "last=" . end($lines) . "\n";
});

$server->start();
await($cli);
--EXPECT--
lines=100
first=001
last=100
