--TEST--
HttpServer: HTTP/2 upload refused mid-part leaves no temp file behind
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--INI--
upload_tmp_dir={PWD}/tmp-067
--FILE--
<?php
/* A file part is written to disk while it arrives. A refusal in the middle
 * of it (here: past setMaxBodySize) must delete that unfinished file too, not
 * only the parts already complete. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_h2_skipif.inc';

$dir = __DIR__ . '/tmp-067';
@mkdir($dir, 0700, true);

$port   = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setMaxBodySize(64 * 1024)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$server->addHttpHandler(function ($req, $resp) {
    $resp->setStatusCode(200)->setBody(json_encode($req->getPost()));
});

$upload = tempnam(sys_get_temp_dir(), 'h2big_');
file_put_contents($upload, str_repeat('u', 256 * 1024));

$client = spawn(function () use ($port, $server, $upload) {
    usleep(30000);
    [, $out] = h2_curl(['--http2-prior-knowledge', '-sS', '--max-time', '5', '-o', h2_dev_null(),
        '-F', "f=@$upload", "http://127.0.0.1:$port/"], true);

    echo 'client: ', str_contains((string)$out, 'ENHANCE_YOUR_CALM') ? 'stream refused' : $out, "\n";
    $server->stop();
});

$server->start();
await($client);
@unlink($upload);

$left = array_values(array_diff(scandir($dir), ['.', '..']));
echo 'files left: ', count($left), "\n";

foreach ($left as $name) {
    @unlink("$dir/$name");
}

@rmdir($dir);
?>
--EXPECT--
client: stream refused
files left: 0
