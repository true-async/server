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
    $curl = sprintf("curl --http1.1 -sS --max-time 5 -w ' %%{http_code}' http://127.0.0.1:%d/ ", $port);
    $fields = static fn(int $n) => implode(' ', array_map(static fn($i) => "-F k$i=v", range(1, $n)));

    echo 'five fields: ', trim((string)shell_exec($curl . $fields(5) . ' 2>&1')), "\n";
    echo 'six fields: ', substr(trim((string)shell_exec($curl . $fields(6) . ' -o /dev/null 2>&1')), -3), "\n";
    echo 'file past the limit: ', substr(trim((string)shell_exec($curl . "-F 'f=@$big' -o /dev/null 2>&1")), -3), "\n";
    echo 'chunked past the limit: ', substr(trim((string)shell_exec($curl
        . "-H 'Transfer-Encoding: chunked' -F 'f=@$big' -o /dev/null 2>&1")), -3), "\n";

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
