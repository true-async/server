--TEST--
HttpResponse::send() — HTTP/1.1 chunked streaming (PLAN_STREAMING Phase 2)
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

$port = 19820 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)->setHeader('Content-Type', 'text/plain');
    for ($i = 1; $i <= 5; $i++) {
        $res->send("chunk-$i\n");
    }
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http1.1 -i -s --max-time 3 http://127.0.0.1:%d/',
        $port
    );
    $out = []; exec($cmd, $out, $rc);

    echo "rc=$rc\n";

    $has_te = false;
    $has_cl = false;
    $body_started = false;
    $body_lines = [];
    foreach ($out as $line) {
        if (!$body_started) {
            if (stripos($line, 'Transfer-Encoding:') === 0 && stripos($line, 'chunked') !== false) {
                $has_te = true;
            }
            if (stripos($line, 'Content-Length:') === 0) {
                $has_cl = true;
            }
            if ($line === '' || $line === "\r") {
                $body_started = true;
            }
        } else {
            $body_lines[] = $line;
        }
    }
    echo "te-chunked=", $has_te ? "yes" : "no", "\n";
    echo "no-content-length=", $has_cl ? "no" : "yes", "\n";
    echo "body=", implode("\n", $body_lines), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
rc=0
te-chunked=yes
no-content-length=yes
body=chunk-1
chunk-2
chunk-3
chunk-4
chunk-5
done
