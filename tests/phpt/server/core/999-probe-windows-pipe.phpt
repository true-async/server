--TEST--
Temporary probe: does a named-pipe listener answer on Windows
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Windows') die('skip probe for Windows'); ?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;
use function Async\delay;

$pid = getmypid();
$path = '\\\\.\\pipe\\ta-probe2-' . $pid;
$tmp = sys_get_temp_dir();
$script = "$tmp\\probe2-client-$pid.php";
$err = "$tmp\\probe2-client-$pid.err";
file_put_contents($script, <<<'PHP'
<?php
$t0 = microtime(true);
$log = fn($m) => fwrite(STDERR, sprintf("[%.3f] %s\n", microtime(true) - $t0, $m));
$f = fopen($argv[1], 'r+b');
if (!$f) { $log('open failed: ' . (error_get_last()['message'] ?? '?')); exit; }
$log('opened');
$n = fwrite($f, "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
$log("wrote $n");
fflush($f);
while (!feof($f)) {
    $c = fread($f, 8192);
    $log('read ' . var_export($c === false ? false : strlen($c), true));
    if ($c === false || $c === '') break;
    echo $c;
}
$log('eof');
PHP);

$logf = fopen("$tmp\\probe2-server-$pid.log", 'w+b');
$config = (new HttpServerConfig())->addUnixListener($path)
    ->setReadTimeout(3)->setWriteTimeout(3)->setStatsEnabled(true)
    ->setLogSinks([['type' => 'stream', 'stream' => $logf, 'format' => 'plain', 'level' => LogSeverity::DEBUG]]);
$server = new HttpServer($config);
$calls = 0;
$server->addHttpHandler(function ($req, $res) use (&$calls) {
    $calls++;
    $res->setStatusCode(200)->setBody('pong')->end();
});

spawn(function () use ($server, $path, $script, $err, &$calls) {
    delay(200);
    $proc = proc_open([PHP_BINARY, '-n', $script, $path],
        [0 => ['file', 'nul', 'r'], 1 => ['pipe', 'w'], 2 => ['file', $err, 'w']], $pipes);
    stream_set_blocking($pipes[1], false);
    $out = '';
    for ($i = 0; $i < 60; $i++) {
        delay(100);
        $c = fread($pipes[1], 65536);
        if (is_string($c)) $out .= $c;
        if (!proc_get_status($proc)['running']) break;
    }
    $running = proc_get_status($proc)['running'];
    echo "client running after wait: ", $running ? 'yes' : 'no', "\n";
    echo "handler calls: $calls\n";
    echo "stats: ", json_encode($server->getStats()['totals'] ?? null), "\n";
    if ($running) proc_terminate($proc);
    $out .= (string) @stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    proc_close($proc);
    echo "client stdout: ", str_replace("\r\n", ' | ', $out), "\n";
    echo "client stderr:\n", @file_get_contents($err);
    $server->stop();
});

$server->start();
rewind($logf);
echo "server log:\n", stream_get_contents($logf);
echo "done\n";
?>
--EXPECT--
always fails: the diff is the probe's report
