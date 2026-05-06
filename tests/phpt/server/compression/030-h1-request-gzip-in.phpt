--TEST--
Compression: gzipped request body decoded; bomb cap → 413; unknown coding → 415 (#8)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (trim((string)shell_exec('command -v gzip')) === '') die('skip gzip(1) not in PATH');
?>
--FILE--
<?php
use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use function Async\spawn;
use function Async\await;
use function Async\delay;

$port = 19400 + getmypid() % 100;

$config = (new HttpServerConfig())
    ->addListener('127.0.0.1', $port)
    ->setReadTimeout(5)
    ->setWriteTimeout(5)
    ->setRequestMaxDecompressedSize(64 * 1024);   /* 64 KiB anti-bomb */

$server = new HttpServer($config);

$server->addHttpHandler(function ($req, $resp) {
    /* Echo the body length the handler observed — proves the parser
     * delivered decoded bytes, not the gzip envelope. */
    $resp->setHeader('Content-Type', 'text/plain')
         ->setBody('len=' . strlen($req->getBody()))
         ->end();
});

/* Build a gzipped payload via gzip(1) so we don't depend on ext/zlib. */
function gzip_string(string $s): string {
    $proc = proc_open(['gzip', '-c'], [
        0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w'],
    ], $pipes);
    fwrite($pipes[0], $s);
    fclose($pipes[0]);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]); fclose($pipes[2]);
    proc_close($proc);
    return $out;
}

function post(string $port, string $body, string $content_encoding): array {
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    $req = "POST /echo HTTP/1.1\r\nHost: x\r\n"
         . "Content-Length: " . strlen($body) . "\r\n"
         . ($content_encoding !== '' ? "Content-Encoding: $content_encoding\r\n" : '')
         . "Connection: close\r\n\r\n";
    fwrite($fp, $req . $body);
    $raw = '';
    while (!feof($fp)) {
        $c = fread($fp, 8192);
        if ($c === '' || $c === false) break;
        $raw .= $c;
    }
    fclose($fp);
    [$head, $body_out] = explode("\r\n\r\n", $raw, 2) + ['', ''];
    $status = (int)(explode(' ', $head)[1] ?? 0);
    return [$status, $body_out];
}

$client = spawn(function () use ($port, $server) {
    delay(20);

    /* 1. gzipped 1 KiB payload — handler must see decoded bytes. */
    $payload = str_repeat("A", 1024);
    $gz = gzip_string($payload);
    [$status, $body] = post($port, $gz, 'gzip');
    echo "gzip status: $status\n";
    echo "gzip body: $body\n";

    /* 2. bomb: 200 KiB of 'A' compresses to ~200 bytes; cap is 64 KiB
     *    → decoder must reject with 413. */
    $bomb = str_repeat("A", 200 * 1024);
    $gzbomb = gzip_string($bomb);
    [$status,] = post($port, $gzbomb, 'gzip');
    echo "bomb status: $status\n";

    /* 3. unknown coding → 415. */
    [$status,] = post($port, 'whatever', 'br');
    echo "unknown status: $status\n";

    /* 4. identity coding → no-op decode, handler sees raw body. */
    [$status, $body] = post($port, "hello", 'identity');
    echo "identity status: $status\n";
    echo "identity body: $body\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);
echo "Done\n";
?>
--EXPECT--
gzip status: 200
gzip body: len=1024
bomb status: 413
unknown status: 415
identity status: 200
identity body: len=5
Done
