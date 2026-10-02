--TEST--
HttpServer: HTTP/2 refuses a multipart body its parser rejects, while the body arrives
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* HTTP/2 feeds a multipart body to its parser frame by frame, as HTTP/1 does,
 * so a body the parser rejects refuses the stream and the handler reads no
 * form. Whether the handler has started by then depends on when the frames
 * land: dispatch happens at the end of the headers. A CR not followed by LF
 * after a boundary is such a body. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';
require_once __DIR__ . '/_h2_skipif.inc';

$port   = tas_free_port();
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5));

$seen = null;

$server->addHttpHandler(function ($req, $resp) use (&$seen) {
    try {
        $seen = 'form ' . json_encode($req->getPost());
    } catch (\Throwable $e) {
        $seen = get_class($e) . ' ' . $e->getCode();
        throw $e;
    }

    $resp->setStatusCode(200)->setBody('read');
});

$body = tempnam(sys_get_temp_dir(), 'h2bad_');
file_put_contents($body, "--bnd\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--bnd\rX");

$client = spawn(function () use ($port, $server, $body) {
    usleep(30000);
    [, $out] = h2_curl(['--http2-prior-knowledge', '-sS', '--max-time', '5', '-o', h2_dev_null(),
        '-w', '%{http_code}', '-H', 'Content-Type: multipart/form-data; boundary=bnd',
        '--data-binary', "@$body", "http://127.0.0.1:$port/form"], true);

    echo 'client: ', str_contains($out, 'CANCEL') ? 'stream reset' : $out, "\n";
    $server->stop();
});

$server->start();
await($client);
@unlink($body);

echo 'handler read a form: ', str_starts_with((string)$seen, 'form') ? $seen : 'no', "\n";
?>
--EXPECT--
client: stream reset
handler read a form: no
