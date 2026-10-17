--TEST--
Rate limit (#414): HTTP/1 refuses a POST before its body, drops the body unbuffered, and serves what is pipelined behind it
--EXTENSIONS--
true_async_server
true_async
--INI--
upload_tmp_dir={PWD}/tmp-090
--FILE--
<?php
/* A method static never serves gets its verdict when the headers end, before
 * any body byte: the body of a refused request is read and dropped, so a
 * multipart upload writes no temporary file and a chunked one is never
 * queued, and the 429 goes out once the body is over, on a connection that
 * stays open for the request pipelined behind it. A refused body still counts
 * against setMaxBodySize(). */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\RateLimit;
use function Async\spawn;
use function Async\delay;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_rate_limit.inc';

$dir = __DIR__ . '/tmp-090';
@mkdir($dir, 0700, true);

$port = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(64 * 1024)
    ->setReadTimeout(10)
    ->setWriteTimeout(10));

$server->addRateLimit((new RateLimit(1, 60))->methods('post', 'PUT')->paths('/limited'));

$handled = [];
$server->addHttpHandler(function ($req, $res) use (&$handled) {
    $handled[] = $req->getMethod() . ' ' . $req->getPath();
    $files = $req->getFiles();
    $res->setBody($req->getMethod() . ' body=' . strlen($req->getBody()) . ' files=' . count($files));
});

$files_now = fn() => count(array_diff(scandir($dir), ['.', '..']));

$boundary = 'rl090';
$part_head = "--$boundary\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a.bin\"\r\n"
           . "Content-Type: application/octet-stream\r\n\r\n";
$part_tail = "\r\n--$boundary--\r\n";
$file_bytes = str_repeat('x', 20000);
$multipart_len = strlen($part_head) + strlen($file_bytes) + strlen($part_tail);

/* Sends a multipart upload in two halves and reports how many temporary files
 * exist between them. */
$upload = function ($fp, string $path) use ($part_head, $part_tail, $file_bytes, $multipart_len, $boundary, $files_now) {
    fwrite($fp, "POST $path HTTP/1.1\r\nHost: t\r\nContent-Type: multipart/form-data; boundary=$boundary\r\n"
              . "Content-Length: $multipart_len\r\n\r\n" . $part_head . substr($file_bytes, 0, 10000));
    delay(200);
    $mid = $files_now();
    fwrite($fp, substr($file_bytes, 10000) . $part_tail);
    return "files mid-upload=$mid " . rl_describe(rl_read_response($fp));
};

spawn(function () use ($server, $port, &$handled, $upload, $files_now) {
    delay(50);
    $fp = rl_connect($port);

    /* The control: an upload under no rule has its file on disk mid-way. */
    echo "free upload: ", $upload($fp, '/free'), "\n";

    fwrite($fp, "POST /limited HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nhello");
    echo "first post: ", rl_describe(rl_read_response($fp)), "\n";

    /* A refused POST and a GET in one write. */
    fwrite($fp, "POST /limited HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n\r\nworld"
              . "GET /after HTTP/1.1\r\nHost: t\r\n\r\n");
    echo "pipelined post: ", rl_describe(rl_read_response($fp)), "\n";
    echo "pipelined get: ", rl_describe(rl_read_response($fp)), "\n";

    /* Chunked, refused mid-body: nothing is answered until the body ends. */
    fwrite($fp, "PUT /limited/x HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n");
    delay(200);
    $r = [$fp]; $w = $e = null;
    echo "answered mid-body: ", stream_select($r, $w, $e, 0, 100000) > 0 ? 'yes' : 'no', "\n";
    fwrite($fp, "6\r\n world\r\n0\r\n\r\nGET /after-chunked HTTP/1.1\r\nHost: t\r\n\r\n");
    echo "chunked put: ", rl_describe(rl_read_response($fp)), "\n";
    echo "get after it: ", rl_describe(rl_read_response($fp)), "\n";

    /* A refused upload writes nothing to disk. */
    echo "limited upload: ", $upload($fp, '/limited/upload'), "\n";
    echo "get after upload: ", rl_describe(rl_get($fp, '/last')), "\n";
    fclose($fp);

    /* The body limit holds for a refused request too. */
    $fp = rl_connect($port);
    fwrite($fp, "POST /limited HTTP/1.1\r\nHost: t\r\nContent-Length: 100000\r\n\r\n");
    echo "over the body limit: ", rl_read_response($fp)['status'] ?? 'closed', "\n";
    fclose($fp);

    echo "files left=", $files_now(), "\n";
    echo "handled=", implode(', ', $handled), "\n";
    $server->stop();
});

$server->start();
echo "done\n";
?>
--CLEAN--
<?php
$dir = __DIR__ . '/tmp-090';
foreach (glob("$dir/*") ?: [] as $f) { @unlink($f); }
@rmdir($dir);
?>
--EXPECT--
free upload: files mid-upload=1 200 body=POST body=0 files=1
first post: 200 body=POST body=5 files=0
pipelined post: 429 retry-after=60 body=Too Many Requests
pipelined get: 200 body=GET body=0 files=0
answered mid-body: no
chunked put: 429 retry-after=60 body=Too Many Requests
get after it: 200 body=GET body=0 files=0
limited upload: files mid-upload=0 429 retry-after=60 body=Too Many Requests
get after upload: 200 body=GET body=0 files=0
over the body limit: 413
files left=0
handled=POST /free, POST /limited, GET /after, GET /after-chunked, GET /last
done
