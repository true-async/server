--TEST--
HttpServer: HTTP/1 refuses a multipart form past the body limit or past max_input_vars
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!shell_exec('which curl')) die('skip curl not installed');
?>
--INI--
max_input_vars=5
upload_tmp_dir={PWD}/tmp-059
--FILE--
<?php
/* A multipart body is written to its processor instead of being buffered, and
 * the Content-Length check sat on the buffering branch only: a 256 KiB upload
 * passed a 64 KiB setMaxBodySize(). Fields past max_input_vars were dropped
 * silently. Both answer before the handler runs now, and the refused upload
 * leaves no temp file. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

$dir = __DIR__ . '/tmp-059';
@mkdir($dir, 0700, true);

$port   = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(64 * 1024)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(200)->setBody(count($req->getPost()) . ' fields, ' . count($req->getFiles()) . ' files');
});

$big = tempnam(sys_get_temp_dir(), 'h1big_');
file_put_contents($big, str_repeat('u', 256 * 1024));

$client = spawn(function () use ($port, $server, $big) {
    usleep(30000);
    /* Each argument quoted alone, since cmd.exe knows no single quotes; the
     * status format sits in a file, because escapeshellarg() on Windows turns
     * % into a space, and the body goes to a file rather than /dev/null. */
    $format = tempnam(sys_get_temp_dir(), 'h1fmt_');
    file_put_contents($format, ' %{http_code}');
    $sink = tempnam(sys_get_temp_dir(), 'h1out_');
    $curl = static fn(array $args): string => trim((string)shell_exec(sprintf(
        'curl --http1.1 -sS --max-time 5 -w %s http://127.0.0.1:%d/ ', escapeshellarg("@$format"), $port)
        . implode(' ', array_map('escapeshellarg', $args)) . ' 2>&1'));
    $fields = static fn(int $n) => array_merge(...array_map(static fn($i) => ['-F', "k$i=v"], range(1, $n)));

    echo 'five fields: ', $curl($fields(5)), "\n";
    echo 'six fields: ', substr($curl([...$fields(6), '-o', $sink]), -3), "\n";
    echo 'file past the limit: ', substr($curl(['-F', "f=@$big", '-o', $sink]), -3), "\n";
    echo 'chunked past the limit: ', substr($curl(['-H', 'Transfer-Encoding: chunked', '-F', "f=@$big", '-o', $sink]), -3), "\n";

    @unlink($format);
    @unlink($sink);
    $server->stop();
});

$server->start();
await($client);
@unlink($big);

echo 'uploads left: ', count(array_diff(scandir($dir), ['.', '..'])), "\n";
array_map('unlink', glob("$dir/*"));
@rmdir($dir);
?>
--EXPECT--
five fields: 5 fields, 0 files 200
six fields: 400
file past the limit: 413
chunked past the limit: 413
uploads left: 0
