--TEST--
StaticHandler: HTTP/2 plaintext fast-path (issue #13 step 2)
--EXTENSIONS--
true_async_server
true_async
--SKIPIF--
<?php
require __DIR__ . '/../h2/_h2_skipif.inc';
h2_skipif(['curl_h2' => true]);
?>
--FILE--
<?php
/* H2 static delivery — exercises h2_stream_send_static_response.
 *
 * 200 with body, ETag/304 conditional, HEAD, single byte-range → 206,
 * 416 unsatisfiable range, 404 outside-mount (dotfile policy default).
 *
 * Client speaks h2c via curl --http2-prior-knowledge — no ALPN /
 * TLS plumbing in this test, just the H2 wire framing on plaintext
 * port. */

use TrueAsync\HttpServer;
use TrueAsync\HttpServerConfig;
use TrueAsync\StaticHandler;
use function Async\spawn;
use function Async\await;

$root = sys_get_temp_dir() . '/static-h2-' . getmypid() . '-' . bin2hex(random_bytes(4));
mkdir($root, 0700, true);
$body = '';
for ($i = 0; $i < 4096; $i++) $body .= chr(($i & 0x3f) + 0x40);
file_put_contents("$root/data.bin", $body);
file_put_contents("$root/index.html", "<h1>root</h1>");

register_shutdown_function(function() use ($root) {
    @unlink("$root/data.bin");
    @unlink("$root/index.html");
    @rmdir($root);
});

$port = 19940 + getmypid() % 50;
$config = (new HttpServerConfig())->addListener('127.0.0.1', $port);
$server = new HttpServer($config);
$server->addStaticHandler(
    (new StaticHandler('/s/', $root))->setIndexFiles('index.html')
);

$client = spawn(function() use ($port, $server, $body) {
    /* Wait for the listener. */
    for ($i = 0; $i < 100; $i++) {
        $fp = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 0.05);
        if ($fp) { fclose($fp); break; }
        usleep(20000);
    }

    $h2 = function(array $extra_args, string $path) use ($port) {
        /* curl options: prior-knowledge h2c, dump headers + body
         * separately so we can parse ":status" and Content-Length
         * cleanly even on 206 / 304. */
        $args = ['--http2-prior-knowledge', '-sS', '-i', '--max-time', '3'];

        /* Route request headers through a curl --config file rather than
         * shell-quoted -H. An ETag is `W/"hex"`, and PHP's escapeshellarg()
         * on Windows cannot carry a double quote — it replaces every " with
         * a space, so `If-None-Match: W/"hex"` would reach curl as
         * `W/ hex `. Leading/trailing whitespace makes that a malformed
         * HTTP/2 field value (RFC 9113 §8.2.1), which the server correctly
         * rejects with a stream PROTOCOL_ERROR. curl config files use curl's
         * own (shell-independent) quoting, so the quotes survive everywhere. */
        $rest = []; $cfg_lines = []; $cfg = null;
        for ($i = 0, $n = count($extra_args); $i < $n; $i++) {
            if ($extra_args[$i] === '-H' && $i + 1 < $n) {
                $val = $extra_args[++$i];
                $esc = str_replace(['\\', '"'], ['\\\\', '\\"'], $val);
                $cfg_lines[] = 'header = "' . $esc . '"';
            } else {
                $rest[] = $extra_args[$i];
            }
        }
        if ($cfg_lines) {
            $cfg = tempnam(sys_get_temp_dir(), 'h2cfg');
            file_put_contents($cfg, implode("\n", $cfg_lines) . "\n");
            $args[] = '--config';
            $args[] = $cfg;
        }
        foreach ($rest as $a) $args[] = $a;
        $args[] = "http://127.0.0.1:$port$path";
        $cmd = 'curl ' . implode(' ', array_map('escapeshellarg', $args));
        $out = []; $rc = 0;
        exec($cmd . ' 2>&1', $out, $rc);
        if ($cfg !== null) @unlink($cfg);
        $resp = implode("\n", $out);
        /* Parse a curl -i response: header block ends at the blank
         * line; HTTP/2 status line is "HTTP/2 NNN". */
        $head_end = strpos($resp, "\n\n");
        if ($head_end === false) $head_end = strpos($resp, "\r\n\r\n");
        $head = $head_end === false ? $resp : substr($resp, 0, $head_end);
        $b    = $head_end === false ? ''   : substr($resp, $head_end + 2);
        if (str_starts_with($b, "\n")) $b = substr($b, 1);
        $lines = preg_split('/\r?\n/', $head);
        $status = $lines[0] ?? '';
        $h = [];
        foreach (array_slice($lines, 1) as $l) {
            if (preg_match('/^([^:]+):\s*(.*)$/', $l, $m)) {
                $h[strtolower(trim($m[1]))] = trim($m[2]);
            }
        }
        return [$rc, $status, $h, $b];
    };

    /* 200 + body. */
    [$rc, $st, $h, $b] = $h2([], '/s/data.bin');
    echo "get200 rc=$rc status=$st cl=", ($h['content-length'] ?? '-'),
         " bodylen=", strlen(rtrim($b, "\n")),
         " match=", (int)(rtrim($b, "\n") === $body), "\n";
    $etag = $h['etag'] ?? '';

    /* If-None-Match → 304, no body. */
    [$rc, $st, $h, $b] = $h2(['-H', "If-None-Match: $etag"], '/s/data.bin');
    echo "cond rc=$rc status=$st bodylen=", strlen(rtrim($b, "\n")), "\n";

    /* HEAD — curl prints headers but no body. */
    [$rc, $st, $h, $b] = $h2(['-I'], '/s/data.bin');
    echo "head rc=$rc status=$st cl=", ($h['content-length'] ?? '-'),
         " bodylen=", strlen(rtrim($b, "\n")), "\n";

    /* Single byte-range → 206. Slice bytes 10..19 inclusive. */
    [$rc, $st, $h, $b] = $h2(['-H', 'Range: bytes=10-19'], '/s/data.bin');
    $expected_slice = substr($body, 10, 10);
    $actual_slice = rtrim($b, "\n");
    echo "range206 rc=$rc status=$st cr=", ($h['content-range'] ?? '-'),
         " match=", (int)($actual_slice === $expected_slice), "\n";

    /* Unsatisfiable range → 416. */
    [$rc, $st, $h, $b] = $h2(['-H', 'Range: bytes=999999-1000000'], '/s/data.bin');
    echo "range416 rc=$rc status=$st cr=", ($h['content-range'] ?? '-'), "\n";

    /* 404 — file not in mount. */
    [$rc, $st, $h, $b] = $h2([], '/s/missing.bin');
    echo "miss rc=$rc status=$st\n";

    /* Index — confirms directory → index.html resolution still works. */
    [$rc, $st, $h, $b] = $h2([], '/s/');
    echo "index rc=$rc status=$st bodyhas=",
         (int)(strpos($b, '<h1>root</h1>') !== false), "\n";

    $server->stop();
});

$server->start();
await($client);
echo "done\n";
--EXPECT--
get200 rc=0 status=HTTP/2 200 cl=- bodylen=4096 match=1
cond rc=0 status=HTTP/2 304 bodylen=0
head rc=0 status=HTTP/2 200 cl=- bodylen=0
range206 rc=0 status=HTTP/2 206 cr=bytes 10-19/4096 match=1
range416 rc=0 status=HTTP/2 416 cr=bytes */4096
miss rc=0 status=HTTP/2 404
index rc=0 status=HTTP/2 200 bodyhas=1
done
