--TEST--
Rate limit (#414): with rules on, a request carrying Upgrade has its body dropped unread, takes no verdict and spends no token
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\WebSocket')) die('skip needs a WebSocket build');
?>
--INI--
upload_tmp_dir={PWD}/tmp-109
--FILE--
<?php
/* In a WebSocket build every request with an Upgrade header goes to the
 * upgrade path, which answers it without reading the body: 426 here, where
 * no WebSocket handler is registered (core/105 has the 405 of one). The rate
 * limit leaves the verdict to the dispatch side for such a request, and the
 * body is dropped while it arrives: a multipart upload writes no temporary
 * file, nothing is counted, and the POST after it still has its token. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$dir = __DIR__ . '/tmp-109';
@mkdir($dir, 0700, true);

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit((new RateLimit(1, 60))->methods('POST')->name('posts'));
$server->addHttpHandler(fn($req, $res) => $res->setBody('handler ' . count($req->getFiles())));

$files_now = fn() => count(array_diff(scandir($dir), ['.', '..']));

spawn(function () use ($server, $port, $files_now) {
    delay(50);
    $boundary = 'rl109';
    $head = "--$boundary\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a.bin\"\r\n"
          . "Content-Type: application/octet-stream\r\n\r\n";
    $tail = "\r\n--$boundary--\r\n";
    $data = str_repeat('x', 20000);
    $length = strlen($head) + strlen($data) + strlen($tail);

    $fp = rl_connect($port);
    fwrite($fp, "POST /up HTTP/1.1\r\nHost: t\r\nUpgrade: x\r\nConnection: Upgrade\r\n"
              . "Content-Type: multipart/form-data; boundary=$boundary\r\nContent-Length: $length\r\n\r\n"
              . $head . substr($data, 0, 10000));
    delay(200);
    echo "files mid-upload: ", $files_now(), "\n";
    fwrite($fp, substr($data, 10000) . $tail);
    echo "upgrade POST: ", rl_read_response($fp)['status'] ?? 'closed', "\n";
    fclose($fp);

    $fp = rl_connect($port);
    fwrite($fp, "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nhi");
    echo "POST after it: ", rl_describe(rl_read_response($fp)), "\n";
    fclose($fp);

    $t = $server->getTelemetry();
    echo "limited=", $t['requests_rate_limited_total'], " files left=", $files_now(), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/tmp-109';
foreach (glob("$dir/*") ?: [] as $f) { @unlink($f); }
@rmdir($dir);
?>
--EXPECT--
files mid-upload: 0
upgrade POST: 426
POST after it: 200 body=handler 0
limited=0 files left=0
done
