--TEST--
Rate limit (#414): without WebSocket support a POST carrying Upgrade is judged before its body like any other POST
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (class_exists('TrueAsync\WebSocket')) die('skip needs a build without WebSocket (core/109 covers this one)');
?>
--INI--
upload_tmp_dir={PWD}/tmp-112
--FILE--
<?php
/* Without WebSocket support no upgrade path claims a request, so a POST with
 * an Upgrade header is an ordinary POST: it is judged when its headers end,
 * and once over the limit its multipart body is dropped unread, no
 * temporary file appears, and the answer is 429. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$dir = __DIR__ . '/tmp-112';
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
    fwrite($fp, "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nhi");
    echo "POST: ", rl_describe(rl_read_response($fp)), "\n";
    fclose($fp);

    $fp = rl_connect($port);
    fwrite($fp, "POST /up HTTP/1.1\r\nHost: t\r\nUpgrade: x\r\nConnection: Upgrade\r\n"
              . "Content-Type: multipart/form-data; boundary=$boundary\r\nContent-Length: $length\r\n\r\n"
              . $head . substr($data, 0, 10000));
    delay(200);
    echo "files mid-upload: ", $files_now(), "\n";
    fwrite($fp, substr($data, 10000) . $tail);
    echo "upgrade POST: ", rl_describe(rl_read_response($fp)), "\n";
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
$dir = __DIR__ . '/tmp-112';
foreach (glob("$dir/*") ?: [] as $f) { @unlink($f); }
@rmdir($dir);
?>
--EXPECT--
POST: 200 body=handler 0
files mid-upload: 0
upgrade POST: 429 retry-after=60 body=Too Many Requests
limited=1 files left=0
done
