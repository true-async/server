--TEST--
Compression H1 buffered: gzip when Accept-Encoding: gzip + whitelisted MIME (#8)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (trim((string)shell_exec('command -v gunzip')) === '') die('skip gunzip(1) not in PATH');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19200 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $resp) {
    $resp->setHeader('Content-Type', 'text/html; charset=utf-8')
         ->setBody(str_repeat("Hello, gzip!\n", 200))   /* > 1024 byte threshold */
         ->end();
});

/* Issue a raw H1 request with Accept-Encoding: gzip and parse the
 * response head. We don't need a full HTTP client — we only need the
 * status line, headers, and the gzipped body. */
function fetch(string $host, int $port, string $accept_encoding): array {
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
        "GET / HTTP/1.1\r\n"
      . "Host: x\r\n"
      . ($accept_encoding !== '' ? "Accept-Encoding: $accept_encoding\r\n" : '')
      . "Connection: close\r\n\r\n");
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    [$head, $body] = explode("\r\n\r\n", $raw, 2);
    $lines = explode("\r\n", $head);
    $status = array_shift($lines);
    $headers = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$status, $headers, $body];
}

$client = spawn(function () use ($port, $server) {
    delay(20);

    /* 1. Accept-Encoding: gzip → compressed */
    [$status, $h, $body] = fetch('127.0.0.1', $port, 'gzip');
    echo "case A status: $status\n";
    echo "case A content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";
    echo "case A vary: ", $h['vary'] ?? '<none>', "\n";
    echo "case A is-gzip-magic: ", (substr($body, 0, 2) === "\x1f\x8b") ? 1 : 0, "\n";
    /* Pipe the gzipped body through gunzip(1) — no ext/zlib needed. */
    $proc = proc_open(['gunzip'], [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ], $pipes);
    fwrite($pipes[0], $body);
    fclose($pipes[0]);
    $decoded = stream_get_contents($pipes[1]);
    fclose($pipes[1]); fclose($pipes[2]);
    proc_close($proc);
    echo "case A round-trip: ", ($decoded === str_repeat("Hello, gzip!\n", 200)) ? "ok" : "MISMATCH", "\n";

    /* 2. No Accept-Encoding (default header semantics: identity only) */
    [, $h, $body] = fetch('127.0.0.1', $port, '');
    echo "case B content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";
    echo "case B body-len: ", strlen($body), "\n";

    /* 3. Accept-Encoding: gzip;q=0 → identity */
    [, $h, $body] = fetch('127.0.0.1', $port, 'gzip;q=0');
    echo "case C content-encoding: ", $h['content-encoding'] ?? '<none>', "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);

echo "Done\n";
?>
--EXPECT--
case A status: HTTP/1.1 200 OK
case A content-encoding: gzip
case A vary: Accept-Encoding
case A is-gzip-magic: 1
case A round-trip: ok
case B content-encoding: <none>
case B body-len: 2600
case C content-encoding: <none>
Done
