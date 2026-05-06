--TEST--
Compression H1 buffered: Brotli encode + decode round-trip via server (#9)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (!in_array('br', TrueAsync\HttpServerConfig::getSupportedEncodings(), true)) {
    die('skip Brotli backend not built (configure with libbrotli-dev)');
}
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19500 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setRequestMaxDecompressedSize(64 * 1024);

$server = new HttpServer($config);

$payload = str_repeat("Brotli round-trip OK!\n", 80);   /* > 1024 byte threshold */

$server->addHttpHandler(function ($req, $resp) use ($payload) {
    if ($req->getPath() === '/echo') {
        /* Reports the length the server delivered after Content-Encoding
         * decoding — proves request-side brotli decoder ran. */
        $resp->setHeader('Content-Type', 'text/plain')
             ->setBody('len=' . strlen($req->getBody()))
             ->end();
        return;
    }
    $resp->setHeader('Content-Type', 'text/html; charset=utf-8')
         ->setBody($payload)
         ->end();
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
function parse(string $raw): array {
    [$head, $body] = explode("\r\n\r\n", $raw, 2) + ['', ''];
    $lines = explode("\r\n", $head);
    $status = array_shift($lines);
    $h = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $h[strtolower(trim($k))] = trim($v);
    }
    return [$status, $h, $body];
}

$client = spawn(function () use ($port, $server, $payload) {
    delay(20);

    /* 1. Accept-Encoding: br → expect br response. */
    $raw = send($port,
        "GET / HTTP/1.1\r\nHost: x\r\n"
      . "Accept-Encoding: br\r\nConnection: close\r\n\r\n");
    [, $h, $body] = parse($raw);
    $encoded = $body;
    echo "case A content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";
    echo "case A vary: ", $h['vary'] ?? '<none>', "\n";
    echo "case A body-not-empty: ", strlen($encoded) > 0 ? 1 : 0, "\n";
    echo "case A body-smaller-than-source: ",
         strlen($encoded) < strlen($payload) ? 1 : 0, "\n";

    /* 2. POST that encoded body back as Content-Encoding: br → request
     * decoder turns it back into $payload, the /echo handler reports
     * the decoded length. End-to-end round-trip via the same server. */
    $raw = send($port,
        "POST /echo HTTP/1.1\r\nHost: x\r\n"
      . "Content-Length: " . strlen($encoded) . "\r\n"
      . "Content-Encoding: br\r\nConnection: close\r\n\r\n"
      . $encoded);
    [, , $body] = parse($raw);
    echo "case B echo: $body\n";
    echo "case B match: ", ($body === 'len=' . strlen($payload)) ? 'ok' : 'MISMATCH', "\n";

    /* 3. Preference order: when both br + gzip offered, br wins. */
    $raw = send($port,
        "GET / HTTP/1.1\r\nHost: x\r\n"
      . "Accept-Encoding: gzip, br\r\nConnection: close\r\n\r\n");
    [, $h,] = parse($raw);
    echo "case C content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";

    /* 4. br;q=0 → server falls back to gzip. */
    $raw = send($port,
        "GET / HTTP/1.1\r\nHost: x\r\n"
      . "Accept-Encoding: br;q=0, gzip\r\nConnection: close\r\n\r\n");
    [, $h,] = parse($raw);
    echo "case D content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
case A content-encoding: br
case A vary: Accept-Encoding
case A body-not-empty: 1
case A body-smaller-than-source: 1
case B echo: len=1760
case B match: ok
case C content-encoding: br
case D content-encoding: gzip
Done
