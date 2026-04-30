--TEST--
HttpServer: server.start / server.stop log records emitted to configured stream (PLAN_LOG.md Step 1)
--EXTENSIONS--
true_async_server
true_async
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\LogSeverity;
use function Async\spawn;

$port = 19960 + getmypid() % 30;
$logfile = sys_get_temp_dir() . "/php-http-server-091-" . getmypid() . ".log";
@unlink($logfile);
$logfh = fopen($logfile, "w+b");
if (!$logfh) { echo "FAIL: cannot open log file\n"; exit(1); }

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setLogSeverity(LogSeverity::INFO)
    ->setLogStream($logfh);

$server = new HttpServer($config);
$server->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });

spawn(function () use ($server) {
    usleep(80000);
    $server->stop();
});

$server->start();

// Force-flush and read log contents.
fflush($logfh);
fclose($logfh);
$log = file_get_contents($logfile);
@unlink($logfile);

// PLAIN format: "TS LEVEL body". We just check key markers are present.
echo "has start: ", (strpos($log, "INFO server.start") !== false ? "yes" : "no"), "\n";
echo "has stop: ",  (strpos($log, "INFO server.stop")  !== false ? "yes" : "no"), "\n";

// Severity OFF — same flow, no records expected.
$logfile2 = sys_get_temp_dir() . "/php-http-server-091b-" . getmypid() . ".log";
@unlink($logfile2);
$logfh2 = fopen($logfile2, "w+b");
$config2 = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->setLogSeverity(LogSeverity::OFF)
    ->setLogStream($logfh2);
$server2 = new HttpServer($config2);
$server2->addHttpHandler(function ($req, $res) { $res->setStatusCode(200)->setBody('OK')->end(); });
spawn(function () use ($server2) { usleep(50000); $server2->stop(); });
$server2->start();
fflush($logfh2);
fclose($logfh2);
$log2 = file_get_contents($logfile2);
@unlink($logfile2);
echo "off-mode size: ", strlen($log2), "\n";

echo "Done\n";
--EXPECT--
has start: yes
has stop: yes
off-mode size: 0
Done
