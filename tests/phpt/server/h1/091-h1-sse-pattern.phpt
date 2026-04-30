--TEST--
HttpResponse::send() — HTTP/1.1 chunked delivers Server-Sent Events
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

$port = 19840 + getmypid() % 100;

$server = new HttpServer(
    (new HttpServerConfig())
        ->addListener('127.0.0.1', $port)
        ->setReadTimeout(10)
        ->setWriteTimeout(10)
);

$server->addHttpHandler(function ($req, $res) {
    $res->setStatusCode(200)
        ->setHeader('Content-Type', 'text/event-stream')
        ->setHeader('Cache-Control', 'no-cache');
    $res->send("data: alpha\n\n");
    $res->send("data: bravo\n\n");
    $res->send("data: charlie\n\n");
    $res->end();
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);
    $cmd = sprintf(
        'curl --http1.1 -i -s -N --max-time 3 http://127.0.0.1:%d/events',
        $port
    );
    $out = []; exec($cmd, $out, $rc);

    $has_ct = false;
    $has_te = false;
    $body_started = false;
    $events = 0;
    foreach ($out as $line) {
        if (!$body_started) {
            if (stripos($line, 'Content-Type:') === 0 && stripos($line, 'text/event-stream') !== false) {
                $has_ct = true;
            }
            if (stripos($line, 'Transfer-Encoding:') === 0 && stripos($line, 'chunked') !== false) {
                $has_te = true;
            }
            if ($line === '' || $line === "\r") {
                $body_started = true;
            }
        } else {
            if (strpos($line, 'data:') === 0) {
                $events++;
            }
        }
    }
    echo "rc=$rc ct=", $has_ct?"yes":"no", " te=", $has_te?"yes":"no", " events=$events\n";

    $t = $server->getTelemetry();
    echo "sends=", $t['stream_send_calls_total'], " bytes=", $t['stream_bytes_sent_total'], "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
?>
--EXPECT--
rc=0 ct=yes te=yes events=3
sends=3 bytes=41
done
