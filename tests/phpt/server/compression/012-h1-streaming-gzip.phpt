--TEST--
Compression H1 streaming: chunked + gzip round-trip (#8)
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

$port = 19300 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5);

$server = new HttpServer($config);

/* Streaming handler emits the same payload over four send() chunks +
 * an end() finaliser. Compression wrapper must produce a single valid
 * gzip stream regardless of chunk boundaries. */
$payload = str_repeat("Hello, streaming gzip!\n", 100);

$server->addHttpHandler(function ($req, $resp) use ($payload) {
    $resp->setHeader('Content-Type', 'text/html');
    $q = strlen($payload) / 4;
    $resp->send(substr($payload, 0,        $q));
    $resp->send(substr($payload, $q,       $q));
    $resp->send(substr($payload, 2*$q,     $q));
    $resp->end(substr($payload, 3*$q));
});

/* Read the full response from the wire — server signals EOF via
 * the terminator zero-chunk + connection close. */
function fetch_chunked(string $port): array {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
        "GET / HTTP/1.1\r\n"
      . "Host: x\r\n"
      . "Accept-Encoding: gzip\r\n"
      . "Connection: close\r\n\r\n");
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    [$head, $rest] = explode("\r\n\r\n", $raw, 2);
    $lines = explode("\r\n", $head);
    array_shift($lines);
    $headers = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    /* Decode chunked transfer encoding — sequence of "<hex>\r\n<bytes>\r\n"
     * with a terminating "0\r\n\r\n". */
    $body = '';
    $i = 0;
    while ($i < strlen($rest)) {
        $eol = strpos($rest, "\r\n", $i);
        if ($eol === false) break;
        $size = hexdec(substr($rest, $i, $eol - $i));
        $i = $eol + 2;
        if ($size === 0) break;
        $body .= substr($rest, $i, $size);
        $i += $size + 2;  /* trailing \r\n after chunk */
    }
    return [$headers, $body];
}

$client = spawn(function () use ($port, $server, $payload) {
    delay(20);

    [$h, $body] = fetch_chunked($port);
    echo "transfer-encoding: ", $h['transfer-encoding'] ?? '<none>', "\n";
    echo "content-encoding: ",  $h['content-encoding']  ?? '<none>', "\n";
    echo "vary: ",              $h['vary']              ?? '<none>', "\n";
    echo "content-length: ",    $h['content-length']    ?? '<absent>', "\n";
    echo "is-gzip-magic: ",     (substr($body, 0, 2) === "\x1f\x8b") ? 1 : 0, "\n";

    /* Pipe through gunzip to recover the original payload. */
    $proc = proc_open(['gunzip'], [
        0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w'],
    ], $pipes);
    fwrite($pipes[0], $body);
    fclose($pipes[0]);
    $decoded = stream_get_contents($pipes[1]);
    fclose($pipes[1]); fclose($pipes[2]);
    proc_close($proc);
    echo "round-trip: ", ($decoded === $payload) ? "ok" : "MISMATCH (got " . strlen($decoded) . " bytes vs expected " . strlen($payload) . ")", "\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
transfer-encoding: chunked
content-encoding: gzip
vary: Accept-Encoding
content-length: <absent>
is-gzip-magic: 1
round-trip: ok
Done
