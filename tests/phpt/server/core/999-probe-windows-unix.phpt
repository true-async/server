--TEST--
Temporary probe: what an AF_UNIX listener does on Windows
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php if (PHP_OS_FAMILY !== 'Windows') die('skip probe for Windows'); ?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\delay;

echo "transports: ", implode(',', stream_get_transports()), "\n";

$tmp = sys_get_temp_dir();
$client = $tmp . '\\probe-client.php';
file_put_contents($client, <<<'PHP'
<?php
$p = $argv[1];
$mode = $argv[2];
if ($mode === 'fopen') {
    $f = @fopen($p, 'r+b');
} else {
    $f = @stream_socket_client('unix://' . $p, $errno, $errstr, 2);
}
if (!$f) { echo "open failed: ", error_get_last()['message'] ?? ($errstr ?? '?'); exit; }
fwrite($f, "GET /x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
$r = '';
while (!feof($f)) { $c = fread($f, 8192); if ($c === false || $c === '') break; $r .= $c; }
echo $r;
PHP);

function probe(string $label, string $path, int $workers, array $modes): void
{
    global $client;
    echo "== $label workers=$workers path=$path\n";
    $config = (new HttpServerConfig())->addUnixListener($path)
        ->setReadTimeout(5)->setWriteTimeout(5);
    if ($workers > 1) {
        $config->setWorkers($workers);
    }
    $server = new HttpServer($config);
    $server->addHttpHandler(function ($req, $res) {
        $res->setStatusCode(200)->setBody('pong')->end();
    });
    spawn(function () use ($server, $path, $modes, $workers) {
        global $client;
        delay($workers > 1 ? 600 : 100);
        foreach ($modes as $mode) {
            $out = sys_get_temp_dir() . "\\probe-out-$mode.txt";
            @unlink($out);
            $p = proc_open([PHP_BINARY, '-n', $client, $path, $mode],
                [0 => ['file', 'NUL', 'r'], 1 => ['file', $out, 'w'], 2 => ['file', $out . '.err', 'w']], $pipes);
            for ($i = 0; $i < 50; $i++) {
                delay(100);
                $st = proc_get_status($p);
                if (!$st['running']) break;
            }
            if (proc_get_status($p)['running']) {
                proc_terminate($p);
                echo "[$mode] client hung\n";
            }
            proc_close($p);
            echo "[$mode] ", str_replace("\r\n", " | ", (string) @file_get_contents($out)),
                 (string) @file_get_contents($out . '.err'), "\n";
        }
        if (str_contains($path, '\\pipe\\') === false) {
            echo "file exists: ", file_exists($path) ? 'yes' : 'no', "\n";
        }
        $server->stop();
    });
    try {
        $server->start();
        echo "stopped\n";
    } catch (\Throwable $e) {
        echo "start threw ", get_class($e), ": ", $e->getMessage(), "\n";
    }
}

$pid = getmypid();
probe('temp path', "$tmp\\ta-probe-$pid.sock", 1, ['fopen', 'unix']);
probe('pipe name', "\\\\.\\pipe\\ta-probe-$pid", 1, ['fopen', 'unix']);
probe('pipe name', "\\\\.\\pipe\\ta-probe-w-$pid", 2, ['fopen']);
probe('temp path', "$tmp\\ta-probe-w-$pid.sock", 2, ['unix']);
exec('curl --version 2>&1', $cv);
echo "curl: ", $cv[0] ?? '?', "\n";
?>
--EXPECT--
always fails: the diff is the probe's report
