--TEST--
Worker inbox (#351): retirement releases queued requests without invoking their handlers
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!function_exists('_http_server_worker_inbox_selftest')) die('skip test hooks required');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
$called = 0;
$server = new HttpServer(new HttpServerConfig());
$server->addHttpHandler(function ($req, $res) use (&$called) { $called++; $res->setBody('bad'); });
$result = await(spawn(function () use ($server) {
    return _http_server_worker_inbox_selftest($server, 128, true);
}));
echo 'queued=', $result['expected'], ' dispatched=', $result['received'],
    ' released=', $result['released'], ' handlers=', $called, "\n";
?>
--EXPECT--
queued=128 dispatched=0 released=128 handlers=0
