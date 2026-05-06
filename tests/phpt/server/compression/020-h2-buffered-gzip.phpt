--TEST--
Compression H2: buffered gzip + identity skip + per-stream MIME match (#8)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$port = 19350 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$body = str_repeat("Hello, h2 gzip!\n", 200);  /* > 1024 byte threshold */

$server->addHttpHandler(function ($req, $resp) use ($body) {
    if ($req->getPath() === '/png') {
        $resp->setHeader('Content-Type', 'image/png')->setBody($body);
    } else {
        $resp->setHeader('Content-Type', 'text/html')->setBody($body);
    }
});

$client = spawn(function () use ($port, $server) {
    usleep(30000);

    /* curl over h2 with --compressed → server should gzip the text/html
     * response and curl gunzips before display. We verify by piping
     * curl output through gunzip ourselves to see the response head
     * (which carries content-encoding: gzip). */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -sS -i --max-time 3 '
        . '-H "Accept-Encoding: gzip" '
        . 'http://127.0.0.1:%d/ | head -c 4096',
        $port);
    $out = shell_exec($cmd);
    /* Header section ends at \r\n\r\n. */
    $hdr = explode("\r\n\r\n", $out, 2)[0] ?? '';
    echo "html has CE-gzip: ", (stripos($hdr, "content-encoding: gzip") !== false) ? 1 : 0, "\n";
    echo "html has Vary: ",    (stripos($hdr, "vary: ") !== false) ? 1 : 0, "\n";

    /* PNG path: must NOT be compressed. */
    $cmd = sprintf(
        'curl --http2-prior-knowledge -sS -i --max-time 3 '
        . '-H "Accept-Encoding: gzip" '
        . 'http://127.0.0.1:%d/png | head -c 4096',
        $port);
    $out = shell_exec($cmd);
    $hdr = explode("\r\n\r\n", $out, 2)[0] ?? '';
    echo "png has CE-gzip: ", (stripos($hdr, "content-encoding: gzip") !== false) ? 1 : 0, "\n";

    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
html has CE-gzip: 1
html has Vary: 1
png has CE-gzip: 0
Done
