--TEST--
HttpServer: HTTP/2 refuses a multipart form past max_input_vars while the body arrives
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--INI--
max_input_vars=5
--FILE--
<?php
/* The multipart processor counts fields against max_input_vars and refuses
 * the body on the one past it, so the stream is reset while the body arrives
 * and the handler reads no form. It used to keep the first fields and drop
 * the rest without a word. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

require_once __DIR__ . '/../_free_port.inc';

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

$body = tempnam(sys_get_temp_dir(), 'h2many_');
file_put_contents($body, implode('', array_map(
    static fn($i) => "--bnd\r\nContent-Disposition: form-data; name=\"k$i\"\r\n\r\nv\r\n", range(1, 6))) . "--bnd--\r\n");

$client = spawn(function () use ($port, $server, $body) {
    usleep(30000);
    $out = shell_exec(sprintf(
        "curl --http2-prior-knowledge -sS --max-time 5 -o /dev/null -w '%%{http_code}' "
        . "-H 'Content-Type: multipart/form-data; boundary=bnd' --data-binary @%s "
        . "http://127.0.0.1:%d/form 2>&1", escapeshellarg($body), $port));

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
