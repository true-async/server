--TEST--
Compression: encoder pool reuse — many sequential gzip responses with distinct bodies all round-trip cleanly
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
if (!class_exists('TrueAsync\HttpServerConfig')) die('skip http_server not loaded');
if (PHP_OS_FAMILY === 'Windows') die('skip requires a POSIX gzip/gunzip CLI for the proc_open round-trip');
if (trim((string)shell_exec('command -v gunzip 2>/dev/null')) === '') die('skip gunzip(1) not in PATH');
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
    ->setWriteTimeout(5)
    ->setCompressionEnabled(true)
    ->setCompressionLevel(6);

$server = new HttpServer($config);

/* The handler echoes a body that depends on the query string, so every
 * request through the pool sees genuinely different input bytes. If the
 * pool's reset() ever fails to scrub deflate state we'd see corrupted
 * output on the second-and-later requests. */
$server->addHttpHandler(function ($req, $resp) {
    $tag = (string)($req->getQuery()['tag'] ?? 'default');
    /* Body needs to exceed the 1024-byte threshold to actually engage
     * the encoder. Mix the tag in repeatedly so identical tags can't
     * coincidentally compare equal across iterations. */
    $body = str_repeat("payload-{$tag}-", 200);
    $resp->setHeader('Content-Type', 'text/plain; charset=utf-8')
         ->setBody($body)
         ->end();
});

function fetch_gz(string $host, int $port, string $tag): array {
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 2);
    stream_set_timeout($fp, 2);
    fwrite($fp,
        "GET /?tag={$tag} HTTP/1.1\r\n"
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
    [$head, $body] = explode("\r\n\r\n", $raw, 2);
    $lines = explode("\r\n", $head);
    array_shift($lines);   /* status line — we trust it; round-trip is the check */
    $headers = [];
    foreach ($lines as $l) {
        if (strpos($l, ':') === false) continue;
        [$k, $v] = explode(':', $l, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$headers, $body];
}

function gunzip(string $data): string {
    $proc = proc_open(['gunzip'], [
        0 => ['pipe', 'r'],
        1 => ['pipe', 'w'],
        2 => ['pipe', 'w'],
    ], $pipes);
    fwrite($pipes[0], $data);
    fclose($pipes[0]);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]); fclose($pipes[2]);
    proc_close($proc);
    return $out;
}

$client = spawn(function () use ($port, $server) {
    delay(20);

    /* 30 iterations: enough that an empty pool warms up, fills, and is
     * exercised mainly via the reset()-hit path. Each tag is unique so
     * a stale-buffer bug shows up as a content mismatch. */
    $ok = 0;
    $mismatches = [];
    $bad_magic = 0;
    for ($i = 0; $i < 30; $i++) {
        $tag = "iter{$i}-" . bin2hex(random_bytes(3));
        [$h, $body] = fetch_gz('127.0.0.1', $port, $tag);

        if (($h['content-encoding'] ?? '') !== 'gzip') {
            $mismatches[] = "iter $i: not gzip-encoded";
            continue;
        }

        if (substr($body, 0, 2) !== "\x1f\x8b") {
            $bad_magic++;
            continue;
        }

        $decoded = gunzip($body);
        $expected = str_repeat("payload-{$tag}-", 200);

        if ($decoded === $expected) {
            $ok++;
        } else {
            /* Keep the first failure terse — we just want to know the pool broke. */
            $mismatches[] = "iter $i: body mismatch (got " . strlen($decoded)
                . " bytes, expected " . strlen($expected) . ")";
        }
    }

    echo "ok: {$ok}/30\n";
    echo "bad-magic: {$bad_magic}\n";
    echo "mismatches: ", count($mismatches), "\n";
    foreach (array_slice($mismatches, 0, 3) as $m) echo "  - $m\n";

    delay(50);
    $server->stop();
});

$server->start();
await($client);

echo "Done\n";
?>
--EXPECT--
ok: 30/30
bad-magic: 0
mismatches: 0
Done
