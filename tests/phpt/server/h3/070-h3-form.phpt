--TEST--
HttpServer: HTTP/3 forms — url-encoded and multipart bodies reach getPost() and getFiles()
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/_h3_skipif.inc';
h3_skipif(['openssl_cli' => true, 'h3client' => true]);
?>
--FILE--
<?php
/* HTTP/3 starts the handler at the end of the headers, so the handler here
 * reads the form without awaitBody(): a form getter waits for the body itself.
 * The third body is parsed in two 256 KiB slices with a yield between them. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;

$tmp = __DIR__ . '/tmp-070';
@mkdir($tmp, 0700, true);
$cert = $tmp . '/cert.pem';
$key  = $tmp . '/key.pem';
$rc   = 0;
exec(sprintf('openssl req -x509 -newkey rsa:2048 -sha256 -days 1 -nodes '
    . '-subj "/CN=localhost" -keyout %s -out %s 2>/dev/null',
    escapeshellarg($key), escapeshellarg($cert)), $_, $rc);
if ($rc !== 0) { echo "cert gen failed\n"; exit(1); }

$urlencoded = $tmp . '/urlencoded.bin';
file_put_contents($urlencoded, 'a=1&list%5B%5D=2&list%5B%5D=3&map%5Bkey%5D=4');

$boundary  = 'h3bnd';
$multipart = $tmp . '/multipart.bin';
file_put_contents($multipart,
    "--$boundary\r\nContent-Disposition: form-data; name=\"list[]\"\r\n\r\n2\r\n"
    . "--$boundary\r\nContent-Disposition: form-data; name=\"list[]\"\r\n\r\n3\r\n"
    . "--$boundary\r\nContent-Disposition: form-data; name=\"docs[cv]\"; filename=\"cv.txt\"\r\n"
    . "Content-Type: text/plain\r\n\r\nfile-bytes\r\n"
    . "--$boundary--\r\n");

$sliced = $tmp . '/sliced.bin';
file_put_contents($sliced,
    "--$boundary\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\nbefore\r\n"
    . "--$boundary\r\nContent-Disposition: form-data; name=\"big\"; filename=\"big.bin\"\r\n"
    . "Content-Type: application/octet-stream\r\n\r\n" . str_repeat('z', 400 * 1024) . "\r\n"
    . "--$boundary\r\nContent-Disposition: form-data; name=\"after\"\r\n\r\nyes\r\n"
    . "--$boundary--\r\n");

require_once __DIR__ . '/../_free_port.inc';

$port   = tas_free_port_span(2);
$server = new HttpServer((new HttpServerConfig())
    ->addListener('127.0.0.1', $port + 1)
    ->addHttp3Listener('127.0.0.1', $port)
    ->enableTls(true)->setCertificate($cert)->setPrivateKey($key));

$server->addHttpHandler(function ($req, $res) {
    $files = array_map(
        static fn($entry) => is_array($entry)
            ? array_map(static fn($file) => $file->getClientFilename() . ':' . $file->getSize(), $entry)
            : $entry->getClientFilename() . ':' . $entry->getSize(),
        $req->getFiles()
    );

    $res->setStatusCode(200)->setBody(json_encode(['post' => $req->getPost(), 'files' => $files]));
});

$client_bin = __DIR__ . '/../../../h3client/h3client';

$client = spawn(function () use ($server, $port, $client_bin, $urlencoded, $multipart, $sliced, $boundary) {
    usleep(80000);

    $request = static function (string $content_type, string $body_path) use ($client_bin, $port): string {
        $out = shell_exec(sprintf('H3CLIENT_HEADER=%s %s 127.0.0.1 %d /form POST %s 2>&1',
            escapeshellarg('content-type: ' . $content_type),
            escapeshellarg($client_bin), $port, escapeshellarg($body_path))) ?? '';

        return trim(preg_replace('/^STATUS=\d+\n?/m', '', $out));
    };

    echo $request('application/x-www-form-urlencoded', $urlencoded), "\n";
    echo $request("multipart/form-data; boundary=\"$boundary\"", $multipart), "\n";
    echo $request("multipart/form-data; boundary=$boundary", $sliced), "\n";

    $server->stop();
});

$server->start();
await($client);

@unlink($cert); @unlink($key); @unlink($urlencoded); @unlink($multipart); @unlink($sliced); @rmdir($tmp);
echo "done\n";
?>
--EXPECT--
{"post":{"a":"1","list":["2","3"],"map":{"key":"4"}},"files":[]}
{"post":{"list":["2","3"]},"files":{"docs":{"cv":"cv.txt:10"}}}
{"post":{"note":"before","after":"yes"},"files":{"big":"big.bin:409600"}}
done
