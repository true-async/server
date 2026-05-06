--TEST--
Compression H1 streaming: chunked + Brotli round-trip via server (#9)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!in_array('br', TrueAsync\HttpServerConfig::getSupportedEncodings(), true)) {
    die('skip Brotli backend not built');
}
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19700 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setRequestMaxDecompressedSize(64 * 1024);

$server = new HttpServer($config);

/* Big chunked payload — drives several encoder passes inside the
 * streaming wrapper, which is the exact path that exercises
 * br_drain_output's spill branch when the output buffer fills mid-encode. */
$chunk = str_repeat("Streaming Brotli chunk!\n", 64);   /* ~1500 bytes */
$rounds = 8;
$expected = str_repeat($chunk, $rounds);

$server->addHttpHandler(function ($req, $resp) use ($chunk, $rounds) {
    if ($req->getPath() === '/echo') {
        $resp->setHeader('Content-Type', 'text/plain')
             ->setBody('len=' . strlen($req->getBody()))
             ->end();
        return;
    }
    $resp->setHeader('Content-Type', 'text/html');
    for ($i = 0; $i < $rounds; $i++) {
        $resp->send($chunk);
    }
    $resp->end();
});

function send(string $port, string $req): string {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp, $req);
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    return $raw;
}
function parse_h1(string $raw): array {
    [$head, $rest] = explode("\r\n\r\n", $raw, 2) + ['', ''];
    $lines = explode("\r\n", $head);
    array_shift($lines);
    $h = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $h[strtolower(trim($k))] = trim($v);
    }
    /* De-chunk */
    $body = '';
    if (($h['transfer-encoding'] ?? '') === 'chunked') {
        $p = 0;
        while (true) {
            $eol = strpos($rest, "\r\n", $p);
            if ($eol === false) break;
            $size = (int) hexdec(trim(substr($rest, $p, $eol - $p)));
            if ($size === 0) break;
            $p = $eol + 2;
            $body .= substr($rest, $p, $size);
            $p += $size + 2;
        }
    } else {
        $body = $rest;
    }
    return [$h, $body];
}

$client = spawn(function () use ($port, $server, $expected) {
    delay(20);

    /* GET /, Accept-Encoding: br — server emits chunked + brotli. */
    $raw = send($port,
        "GET / HTTP/1.1\r\nHost: x\r\n"
      . "Accept-Encoding: br\r\nConnection: close\r\n\r\n");
    [$h, $encoded] = parse_h1($raw);
    echo "encoding: ", $h['content-encoding'] ?? '<none>', "\n";
    echo "transfer-encoding: ", $h['transfer-encoding'] ?? '<none>', "\n";
    echo "encoded-non-empty: ", strlen($encoded) > 0 ? 1 : 0, "\n";
    echo "ratio<1: ", strlen($encoded) < strlen($expected) ? 1 : 0, "\n";

    /* POST the streamed-encoded body back, server decodes. */
    $raw = send($port,
        "POST /echo HTTP/1.1\r\nHost: x\r\n"
      . "Content-Length: " . strlen($encoded) . "\r\n"
      . "Content-Encoding: br\r\nConnection: close\r\n\r\n"
      . $encoded);
    [, $body] = parse_h1($raw);
    echo "decoded match: ", ($body === 'len=' . strlen($expected)) ? 'ok' : "MISMATCH ($body vs len=" . strlen($expected) . ")", "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
encoding: br
transfer-encoding: chunked
encoded-non-empty: 1
ratio<1: 1
decoded match: ok
Done
