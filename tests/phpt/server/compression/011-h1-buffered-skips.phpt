--TEST--
Compression H1 buffered: skip rules (HEAD, Range, MIME, threshold, opt-out, handler CE) (#8)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19250 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$body = str_repeat("Hello, gzip!\n", 200);  /* 2600 bytes */

$server->addHttpHandler(function ($req, $resp) use ($body) {
    $path = $req->getPath();
    if ($path === '/png') {
        /* Non-whitelisted MIME — should not be compressed even with AE: gzip. */
        $resp->setHeader('Content-Type', 'image/png')->setBody($body)->end();
    } elseif ($path === '/small') {
        /* Below 1024-byte threshold. */
        $resp->setHeader('Content-Type', 'text/html')->setBody('hi')->end();
    } elseif ($path === '/optout') {
        $resp->setHeader('Content-Type', 'text/html')
             ->setNoCompression()
             ->setBody($body)
             ->end();
    } elseif ($path === '/preencoded') {
        /* Handler already set Content-Encoding — server must not double-encode. */
        $resp->setHeader('Content-Type', 'text/html')
             ->setHeader('Content-Encoding', 'br')
             ->setBody($body)
             ->end();
    } elseif ($path === '/204') {
        $resp->setStatusCode(204)->end();
    } else {
        $resp->setHeader('Content-Type', 'text/html')->setBody($body)->end();
    }
});

function fetch(string $port, string $path, array $req_headers): array {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    [$method, $resource] = (str_starts_with($path, 'HEAD '))
        ? ['HEAD', substr($path, 5)] : ['GET', $path];
    $hdr = "$method $resource HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";
    foreach ($req_headers as $k => $v) $hdr .= "$k: $v\r\n";
    fwrite($fp, $hdr . "\r\n");
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    @[$head, $body] = explode("\r\n\r\n", $raw, 2);
    $lines = explode("\r\n", $head);
    array_shift($lines);
    $headers = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$headers, $body ?? ''];
}

$client = spawn(function () use ($port, $server) {
    delay(20);

    [$h] = fetch($port, '/png', ['Accept-Encoding' => 'gzip']);
    echo "png CE: ", $h['content-encoding'] ?? '<none>', "\n";

    [$h] = fetch($port, '/small', ['Accept-Encoding' => 'gzip']);
    echo "small CE: ", $h['content-encoding'] ?? '<none>', "\n";

    [$h] = fetch($port, '/optout', ['Accept-Encoding' => 'gzip']);
    echo "optout CE: ", $h['content-encoding'] ?? '<none>', "\n";

    [$h] = fetch($port, '/preencoded', ['Accept-Encoding' => 'gzip']);
    echo "preencoded CE: ", $h['content-encoding'] ?? '<none>', "\n";

    [$h, $b] = fetch($port, 'HEAD /', ['Accept-Encoding' => 'gzip']);
    echo "HEAD CE: ", $h['content-encoding'] ?? '<none>', "\n";

    [$h] = fetch($port, '/', ['Accept-Encoding' => 'gzip', 'Range' => 'bytes=0-99']);
    echo "range CE: ", $h['content-encoding'] ?? '<none>', "\n";

    [$h] = fetch($port, '/204', ['Accept-Encoding' => 'gzip']);
    echo "204 CE: ", $h['content-encoding'] ?? '<none>', "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);

echo "Done\n";
?>
--EXPECT--
png CE: <none>
small CE: <none>
optout CE: <none>
preencoded CE: br
HEAD CE: <none>
range CE: <none>
204 CE: <none>
Done
